-- AMETHYST profile pictures (Pro only)
-- Run in the Supabase SQL editor after 20261124000000_entitlements.sql.
--
-- Private bucket "avatars"; each user's picture lives at  <user id>/avatar.png
-- Everyone can see/delete their OWN picture, but only active Pro users can upload or replace one.
-- This is enforced by the database, so a modified app can't bypass it.

insert into storage.buckets (id, name, public, file_size_limit, allowed_mime_types)
values ('avatars', 'avatars', false, 1048576, array['image/png', 'image/jpeg'])
on conflict (id) do update
    set public = excluded.public,
        file_size_limit = excluded.file_size_limit,
        allowed_mime_types = excluded.allowed_mime_types;

-- Is the signed-in user an active Pro subscriber?
create or replace function public.is_active_pro()
returns boolean
language sql
stable
security definer
set search_path = ''
as $$
    select exists (
        select 1 from public.entitlements e
        where e.user_id = (select auth.uid())
          and e.is_pro
          and (e.pro_until is null or e.pro_until > now())
    );
$$;

revoke all on function public.is_active_pro() from public;
grant execute on function public.is_active_pro() to authenticated;

drop policy if exists "Avatar: read own" on storage.objects;
create policy "Avatar: read own"
    on storage.objects for select to authenticated
    using (bucket_id = 'avatars' and (storage.foldername(name))[1] = (select auth.uid())::text);

drop policy if exists "Avatar: Pro users upload own" on storage.objects;
create policy "Avatar: Pro users upload own"
    on storage.objects for insert to authenticated
    with check (bucket_id = 'avatars'
                and (storage.foldername(name))[1] = (select auth.uid())::text
                and (select public.is_active_pro()));

drop policy if exists "Avatar: Pro users replace own" on storage.objects;
create policy "Avatar: Pro users replace own"
    on storage.objects for update to authenticated
    using (bucket_id = 'avatars' and (storage.foldername(name))[1] = (select auth.uid())::text)
    with check (bucket_id = 'avatars'
                and (storage.foldername(name))[1] = (select auth.uid())::text
                and (select public.is_active_pro()));

drop policy if exists "Avatar: delete own" on storage.objects;
create policy "Avatar: delete own"
    on storage.objects for delete to authenticated
    using (bucket_id = 'avatars' and (storage.foldername(name))[1] = (select auth.uid())::text);
