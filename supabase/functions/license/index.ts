// AMETHYST license server (Supabase Edge Function "license").
//
// The app calls POST /functions/v1/license with the user's access token. This function checks who the
// user is, reads their row in public.entitlements with the service role, and returns a license signed
// with LICENSE_PRIVATE_KEY (RSASSA-PKCS1-v1_5 / SHA-256):
//     { "license": "<base64url payload>.<base64url signature>" }
// payload = { v: 1, sub, tier, plan, pro_until, iat, exp }   (times in milliseconds, server clock)
// The app verifies it with the public key built into it (Source/LicensePublicKey.h), so licenses can't
// be edited or made up on the user's PC. exp is how long the app keeps working offline.
//
// Setup: supabase secrets set LICENSE_PRIVATE_KEY="$(cat config/license_private.pem)"
//        supabase functions deploy license
// (SUPABASE_URL, SUPABASE_ANON_KEY and SUPABASE_SERVICE_ROLE_KEY are provided by Supabase automatically.)

import { createClient } from "https://esm.sh/@supabase/supabase-js@2.45.4";

const OFFLINE_DAYS = 14;
const DAY = 86_400_000;

const cors = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "authorization, apikey, content-type",
  "Access-Control-Allow-Methods": "POST, OPTIONS",
};

const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), { status, headers: { ...cors, "Content-Type": "application/json" } });

function b64url(bytes: Uint8Array): string {
  let s = "";
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

let signingKey: CryptoKey | null = null;
async function getKey(): Promise<CryptoKey> {
  if (signingKey) return signingKey;
  const pem = Deno.env.get("LICENSE_PRIVATE_KEY");
  if (!pem) throw new Error("LICENSE_PRIVATE_KEY is not set");
  const body = pem.replace(/-----[^-]+-----/g, "").replace(/\s+/g, "");
  const der = Uint8Array.from(atob(body), (c) => c.charCodeAt(0));
  signingKey = await crypto.subtle.importKey("pkcs8", der, { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" }, false, ["sign"]);
  return signingKey;
}

Deno.serve(async (req) => {
  if (req.method === "OPTIONS") return new Response(null, { headers: cors });
  if (req.method !== "POST") return json({ error: "POST only" }, 405);

  const auth = req.headers.get("Authorization") ?? "";
  if (!auth.startsWith("Bearer ")) return json({ error: "Sign in first" }, 401);

  const url = Deno.env.get("SUPABASE_URL")!;
  // who is asking (verifies the user's access token)
  const asUser = createClient(url, Deno.env.get("SUPABASE_ANON_KEY")!, { global: { headers: { Authorization: auth } } });
  const { data: userData, error: userError } = await asUser.auth.getUser();
  if (userError || !userData?.user) return json({ error: "Your session has expired - sign in again" }, 401);
  const userId = userData.user.id;

  // their plan (service role: the table is read-only for users anyway)
  const admin = createClient(url, Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!);
  let { data: row, error } = await admin.from("entitlements").select("is_pro,is_ultra,plan,pro_until").eq("user_id", userId).maybeSingle();
  if (error && /is_ultra/.test(error.message)) {
    ({ data: row, error } = await admin.from("entitlements").select("is_pro,plan,pro_until").eq("user_id", userId).maybeSingle());
  }
  if (error) return json({ error: "Couldn't read your plan" }, 500);

  const now = Date.now();
  const proUntil = row?.pro_until ? Date.parse(row.pro_until) : 0;
  const current = !proUntil || proUntil > now;
  const tier = current ? ((row as { is_ultra?: boolean } | null)?.is_ultra ? 2 : row?.is_pro ? 1 : 0) : 0;

  const payload = {
    v: 1,
    sub: userId,
    tier,
    plan: row?.plan ?? "free",
    pro_until: proUntil,
    iat: now,
    exp: now + OFFLINE_DAYS * DAY,
  };
  const payloadB64 = b64url(new TextEncoder().encode(JSON.stringify(payload)));
  try {
    const sig = new Uint8Array(await crypto.subtle.sign("RSASSA-PKCS1-v1_5", await getKey(), new TextEncoder().encode(payloadB64)));
    return json({ license: `${payloadB64}.${b64url(sig)}` });
  } catch (e) {
    console.error(e);
    return json({ error: "License server isn't set up (signing key missing)" }, 500);
  }
});
