// Creates the RSA key pair that signs AMETHYST licenses.
//   node tools/make-license-keys.js
// - config/license_private.pem   PRIVATE. Paste it into Supabase as the LICENSE_PRIVATE_KEY secret.
//                                 Never commit it (config/ files are git-ignored) and never share it.
// - Source/LicensePublicKey.h    the public half, built into the app to check signatures.
// Running it again makes a NEW pair: licenses signed with the old key stop verifying, so only do
// that if the private key ever leaks (then redeploy the secret and ship an update).
const crypto = require("crypto"), fs = require("fs"), path = require("path");
const root = path.join(__dirname, "..");
const priv = path.join(root, "config", "license_private.pem");
if (fs.existsSync(priv) && !process.argv.includes("--force")) {
  console.log("config/license_private.pem already exists - keeping it (use --force to replace).");
  process.exit(0);
}
const { publicKey, privateKey } = crypto.generateKeyPairSync("rsa", { modulusLength: 2048, publicExponent: 65537 });
fs.mkdirSync(path.dirname(priv), { recursive: true });
fs.writeFileSync(priv, privateKey.export({ type: "pkcs8", format: "pem" }), { mode: 0o600 });
const jwk = publicKey.export({ format: "jwk" });
const hex = Buffer.from(jwk.n, "base64url").toString("hex");
const header = `#pragma once

// Public half of the AMETHYST license signing key (made by tools/make-license-keys.js).
// Public on purpose: it can only CHECK signatures. The private half lives in Supabase secrets.
namespace LicensePublicKey
{
    inline constexpr const char* modulusHex =
${hex.match(/.{1,96}/g).map(s => '        "' + s + '"').join("\n")};
    inline constexpr int exponent = 65537;
}
`;
fs.writeFileSync(path.join(root, "Source", "LicensePublicKey.h"), header);
console.log("Wrote config/license_private.pem (private) and Source/LicensePublicKey.h (public).");
