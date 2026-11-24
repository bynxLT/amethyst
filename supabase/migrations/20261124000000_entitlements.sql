-- AMETHYST Pro entitlements
-- Run this in the Supabase SQL editor (or `supabase db push`).
--
-- One row per user, created automatically at sign-up.
-- Users can READ their own row but never change it: only the service role
-- (your future payment webhook / Edge Function) may set is_pro / pro_until.

create table if not exists public.entitlements (
    user_id     uuid primary key references auth.users (id) on delete cascade,
    is_pro      boolean     not null default false,
    plan        text        not null default 'free',      -- e.g. 'free', 'pro_monthly', 'pro_yearly'
    pro_until   timestamptz,                               -- null = no expiry (e.g. lifetime / manual grant)
    updated_at  timestamptz not null default now()
);

alter table public.entitlements enable row level security;

drop policy if exists "Users can read their own entitlement" on public.entitlements;
create policy "Users can read their own entitlement"
    on public.entitlements
    for select
    to authenticated
    using ((select auth.uid()) = user_id);

-- No insert/update/delete policies on purpose: clients cannot grant themselves Pro.

-- Create the free row whenever someone signs up.
create or replace function public.handle_new_user_entitlement()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
begin
    insert into public.entitlements (user_id) values (new.id)
    on conflict (user_id) do nothing;
    return new;
end;
$$;

drop trigger if exists on_auth_user_created_entitlement on auth.users;
create trigger on_auth_user_created_entitlement
    after insert on auth.users
    for each row execute procedure public.handle_new_user_entitlement();

-- Backfill users who signed up before this migration.
insert into public.entitlements (user_id)
select id from auth.users
on conflict (user_id) do nothing;

-- Keep updated_at fresh.
create or replace function public.touch_entitlement()
returns trigger
language plpgsql
set search_path = ''
as $$
begin
    new.updated_at = now();
    return new;
end;
$$;

drop trigger if exists entitlements_touch on public.entitlements;
create trigger entitlements_touch
    before update on public.entitlements
    for each row execute procedure public.touch_entitlement();

-- To give someone Pro by hand while payments aren't set up yet:
--   update public.entitlements set is_pro = true, plan = 'pro_manual', pro_until = null
--   where user_id = (select id from auth.users where email = 'someone@example.com');
