-- AMETHYST Ultra Pro tier
-- Run in the Supabase SQL editor after the entitlements and avatars migrations.
--
-- Ultra Pro includes everything in Pro, plus Ultra-only effects (AMETHYST TUNE).
-- Like is_pro, only the service role (your future payment webhook) can set it.

alter table public.entitlements
    add column if not exists is_ultra boolean not null default false;

-- Ultra users count as Pro too (e.g. for profile picture uploads).
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
          and (e.is_pro or e.is_ultra)
          and (e.pro_until is null or e.pro_until > now())
    );
$$;

-- To give someone Ultra Pro by hand while payments aren't set up yet:
--   update public.entitlements set is_ultra = true, is_pro = true, plan = 'ultra_manual', pro_until = null
--   where user_id = (select id from auth.users where email = 'someone@example.com');
