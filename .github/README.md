# tw-mod-account-mounts

This is a [Tortoise WoW](https://tortoise-wow.org/) module ported from the AzerothCore module [mod-account-mounts](https://github.com/azerothcore/mod-account-mounts).

## What it does

When a player logs in, the module collects all mount spells known by every character on the same account and teaches them to the logging-in character. Mounts are also re-checked when the player learns a Riding skill or a new mount spell, so relogging is not required. Mounts can be filtered by race, class, and Riding skill.

## Features

- Shares mount spells across all characters on an account.
- Grants shared mounts on login and dynamically when the player learns a Riding skill or a new mount spell.
- Optional race restriction: only learn mounts from characters of the same faction.
- Optional class restriction: only learn mounts from characters of the same class.
- Riding-skill gating: mounts are taught once the character has the Riding skill required by the mount. Tortoise-WoW has two Riding tiers: Apprentice (75) for normal mounts and Journeyman (150) for epic mounts.
- Epic vs normal detection is driven by the world database: `collection_mount` + `item_template` `required_skill_rank` tells us the Riding skill each mount needs. Spells with no item mapping fall back to DBC speed heuristics.
- Optional `AllowAllMounts` mode that teaches every shared mount regardless of Riding skill, bypassing gating entirely.
- Configurable excluded spell list and optional epic/normal overrides.
- Supports `.reload config` for live configuration changes.

## Configuration

Copy the distributed config template after building:

```sh
cp modules/tw-mod-account-mounts/conf/tw-mod-account-mounts.conf.dist \
   <runtime-config-dir>/modules/tw-mod-account-mounts.conf
```

Available options (under `[AccountMounts]`):

| Option | Default | Description |
|---|---|---|
| `Account.Mounts.Enable` | 1 | Enable the module. |
| `Account.Mounts.Announce` | 0 | Announce the module on player login. |
| `Account.Mounts.LimitRace` | 1 | Restrict mounts to the same faction. |
| `Account.Mounts.LimitClass` | 1 | Restrict mounts to the same class. |
| `Account.Mounts.ExcludedSpellIDs` | 0 | Comma-separated list of spell IDs to never share. |
| `Account.Mounts.EpicSpellIDs` | 0 | Always treat these spell IDs as epic (blocked until Journeyman Riding). |
| `Account.Mounts.NormalSpellIDs` | 0 | Override spell IDs to treat as normal (available at Apprentice Riding). Usually not needed because detection uses `collection_mount` + `item_template`. |
| `Account.Mounts.AllowAllMounts` | 0 | When enabled, all shared mounts are taught regardless of Riding skill, skipping gating entirely. |

## Build

The module is discovered automatically by the Tortoise module system. Enable it with:

```sh
cmake -S . -B build -DMODULES=static
# or per-module:
cmake -S . -B build -DMODULE_TW_MOD_ACCOUNT_MOUNTS=static
```

## Credits

- Original AzerothCore module: [mod-account-mounts](https://github.com/azerothcore/mod-account-mounts)
- Original author: ConanHun513
