-- AMETHYST 2026 releases (auto-updates)
-- Run in the Supabase SQL editor.
--
-- Public bucket "releases" holding:
--   latest.json                         { "version", "url", "sha256", "notes" }  read by the app's updater
--   AMETHYST-2026-Setup-<version>.exe   the installer
-- Anyone can download (the app needs no sign-in to update). Nobody can upload with the app's
-- anon key: there are no insert / update / delete policies, so only the dashboard or the
-- service-role key (tools/make-release.ps1) can publish.

insert into storage.buckets (id, name, public, file_size_limit, allowed_mime_types)
values ('releases', 'releases', true, 209715200,
        array['application/json', 'application/octet-stream', 'application/x-msdownload', 'application/vnd.microsoft.portable-executable'])
on conflict (id) do update
    set public = excluded.public,
        file_size_limit = excluded.file_size_limit,
        allowed_mime_types = excluded.allowed_mime_types;

-- public buckets serve files without a select policy; this one makes listing explicit and read-only
drop policy if exists "Releases: anyone can read" on storage.objects;
create policy "Releases: anyone can read"
    on storage.objects for select to anon, authenticated
    using (bucket_id = 'releases');
