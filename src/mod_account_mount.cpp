#include "Chat/Chat.h"
#include "Config/Config.h"
#include "Database/DatabaseEnv.h"
#include "DBCStructure.h"
#include "DBCStores.h"
#include "Log.h"
#include "Player.h"
#include "ScriptObjects.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "SpellDefines.h"
#include "SpellEntry.h"
#include "SpellMgr.h"

#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // Tortoise-WoW has two Riding tiers.
    constexpr uint32 RIDING_SKILL_APPRENTICE = 75;
    constexpr uint32 RIDING_SKILL_JOURNEYMAN = 150;

    // Module configuration, loaded by AccountMountsWorldScript so that
    // .reload config updates it at runtime.
    bool g_enabled = true;
    bool g_announce = false;
    bool g_limitRace = true;
    bool g_limitClass = true;
    bool g_allowAllMounts = false;
    std::set<uint32> g_excludedSpellIds;
    std::set<uint32> g_forcedEpicSpellIds;
    std::set<uint32> g_forcedNormalSpellIds;

    // Maximum required_skill_rank for each mount spell, loaded from
    // collection_mount + item_template. Updated by .reload config.
    std::map<uint32, uint32> g_spellIdToMaxSkillRank;

    bool IsMountSpell(SpellEntry const* spellInfo)
    {
        if (!spellInfo)
            return false;

        return spellInfo->Effect[0] == SPELL_EFFECT_APPLY_AURA &&
               spellInfo->EffectApplyAuraName[0] == SPELL_AURA_MOUNTED;
    }

    // Returns true if the DBC data says this is a 100% speed mount.
    bool HasEpicSpeedInDbc(SpellEntry const* spellInfo)
    {
        if (!spellInfo)
            return false;

        for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (spellInfo->EffectApplyAuraName[i] == SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED &&
                spellInfo->EffectBasePoints[i] >= 90)
            {
                return true;
            }
        }

        return false;
    }

    // Returns the Riding skill required before a character can learn this mount.
    // The authoritative source is collection_mount + item_template (required_skill
    // for Riding is 762; Tortoise-WoW uses ranks 75 and 150). Config overrides
    // and DBC speed heuristics are fallbacks for spells not in the collection table.
    uint32 GetRequiredRidingSkill(uint32 spellId, SpellEntry const* spellInfo)
    {
        if (g_forcedNormalSpellIds.find(spellId) != g_forcedNormalSpellIds.end())
            return RIDING_SKILL_APPRENTICE;

        if (g_forcedEpicSpellIds.find(spellId) != g_forcedEpicSpellIds.end())
            return RIDING_SKILL_JOURNEYMAN;

        // Use the DB-derived required skill rank when available. Tortoise-WoW
        // only has two Riding tiers (Apprentice 75 / Journeyman 150), so any
        // rank above Apprentice is mapped to the epic threshold.
        auto skillItr = g_spellIdToMaxSkillRank.find(spellId);
        if (skillItr != g_spellIdToMaxSkillRank.end() && skillItr->second > 0)
            return (skillItr->second <= RIDING_SKILL_APPRENTICE) ? RIDING_SKILL_APPRENTICE : RIDING_SKILL_JOURNEYMAN;

        if (!spellInfo)
            return RIDING_SKILL_APPRENTICE;

        // A spell that forces 100% speed is treated as requiring epic riding.
        if (spellInfo->Custom & SPELL_CUSTOM_MOUNT_SPEED_100)
            return RIDING_SKILL_JOURNEYMAN;

        // Keeps the DBC speed amount instead of scaling. Treat as epic only
        // if the DBC data itself says 100%.
        if (spellInfo->Custom & SPELL_CUSTOM_IGNORE_RIDING_SKILL_MOUNT_SPEED)
            return HasEpicSpeedInDbc(spellInfo) ? RIDING_SKILL_JOURNEYMAN : RIDING_SKILL_APPRENTICE;

        // Fallback heuristic: 100% DBC speed.
        return HasEpicSpeedInDbc(spellInfo) ? RIDING_SKILL_JOURNEYMAN : RIDING_SKILL_APPRENTICE;
    }

    bool CanLearnMount(uint32 spellId, Player const* player, SpellEntry const* spellInfo)
    {
        if (!player || !spellInfo)
            return false;

        if (g_allowAllMounts)
            return true;

        uint16 ridingSkill = player->GetSkillValue(SKILL_RIDING);
        uint32 requiredSkill = GetRequiredRidingSkill(spellId, spellInfo);
        return ridingSkill >= requiredSkill;
    }

    // Forward declaration used by ShareAccountMounts.
    bool IsAllowedForPlayerClass(uint32 spellId, Player const* player);

    // Prevents re-entry when LearnSpell inside ShareAccountMounts triggers
    // PLAYERHOOK_ON_LEARN_SPELL.
    thread_local bool g_isSharingMounts = false;

    // Collects all mount spells known by every character on the same account
    // and teaches the ones this player is eligible for.
    void ShareAccountMounts(Player* player)
    {
        if (!g_enabled || g_isSharingMounts)
            return;

        g_isSharingMounts = true;

        if (g_announce)
            ChatHandler(player->GetSession()).SendSysMessage("This server is running the |cff4CFF00AccountMounts |rmodule.");

        std::vector<uint32> guids;
        uint32 accountId = player->GetSession()->GetAccountId();

        std::unique_ptr<QueryResult> result(CharacterDatabase.PQuery(
            "SELECT `guid`, `race`, `class` FROM `characters` WHERE `account`=%u",
            accountId));

        if (!result)
        {
            g_isSharingMounts = false;
            return;
        }

        do
        {
            Field* fields = result->Fetch();
            uint32 guid = fields[0].GetUInt32();
            uint8 race = fields[1].GetUInt8();

            bool matches = true;
            matches &= (Player::TeamForRace(race) == Player::TeamForRace(player->GetRace())) || !g_limitRace;

            if (matches)
                guids.push_back(guid);
        } while (result->NextRow());

        std::set<uint32> spellsToLearn;

        for (uint32 guid : guids)
        {
            std::unique_ptr<QueryResult> spellResult(CharacterDatabase.PQuery(
                "SELECT `spell` FROM `character_spell` WHERE `guid`=%u",
                guid));

            if (!spellResult)
                continue;

            do
            {
                uint32 spellId = spellResult->Fetch()[0].GetUInt32();
                spellsToLearn.insert(spellId);
            } while (spellResult->NextRow());
        }

        for (uint32 spellId : spellsToLearn)
        {
            if (g_excludedSpellIds.find(spellId) != g_excludedSpellIds.end())
                continue;

            if (player->HasSpell(spellId))
                continue;

            SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellId);
            if (!spellInfo)
                continue;

            if (!IsMountSpell(spellInfo))
                continue;

            if (!CanLearnMount(spellId, player, spellInfo))
                continue;

            if (!IsAllowedForPlayerClass(spellId, player))
                continue;

            player->LearnSpell(spellId, false);
        }

        g_isSharingMounts = false;
    }

    // Returns true if the spell teaches the Riding skill.
    bool IsRidingSkillSpell(uint32 spellId, SpellEntry const* spellInfo)
    {
        if (!spellInfo)
            spellInfo = sSpellMgr.GetSpellEntry(spellId);

        if (!spellInfo)
            return false;

        for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (spellInfo->Effect[i] == SPELL_EFFECT_SKILL &&
                spellInfo->EffectMiscValue[i] == SKILL_RIDING)
            {
                return true;
            }
        }

        return false;
    }

    // Checks whether a mount spell is usable by the player's class.
    // Normal mounts have no class restriction; class mounts (e.g. warlock
    // felsteed, paladin warhorse) are restricted to their class.
    bool IsAllowedForPlayerClass(uint32 spellId, Player const* player)
    {
        if (!g_limitClass)
            return true;

        SkillLineAbilityMapBounds bounds = sSpellMgr.GetSkillLineAbilityMapBoundsBySpellId(spellId);
        bool hasClassRestriction = false;
        bool playerMatches = false;

        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            SkillLineAbilityEntry const* ability = itr->second;
            if (!ability)
                continue;

            if (ability->classmask != 0)
            {
                hasClassRestriction = true;
                if (ability->classmask & player->GetClassMask())
                    playerMatches = true;
            }
        }

        // If no class restriction is defined, the mount is usable by all classes.
        return !hasClassRestriction || playerMatches;
    }

    void LoadSpellIdSet(std::string const& configValue, std::set<uint32>& outSet)
    {
        outSet.clear();

        if (configValue == "0" || configValue.empty())
            return;

        std::istringstream stream(configValue);
        std::string token;
        while (std::getline(stream, token, ','))
        {
            try
            {
                uint32 spellId = static_cast<uint32>(std::stoul(token));
                if (spellId != 0)
                    outSet.insert(spellId);
            }
            catch (std::exception const&)
            {
                // Ignore malformed entries.
            }
        }
    }

    void LoadMountItemInfo()
    {
        g_spellIdToMaxSkillRank.clear();

        std::unique_ptr<QueryResult> result(WorldDatabase.PQuery(
            "SELECT `cm`.`spellId`, MAX(`it`.`required_skill_rank`) "
            "FROM `collection_mount` `cm` "
            "JOIN `item_template` `it` ON `cm`.`itemId` = `it`.`entry` "
            "GROUP BY `cm`.`spellId`"));

        if (!result)
            return;

        do
        {
            Field* fields = result->Fetch();
            uint32 spellId = fields[0].GetUInt32();
            uint32 maxSkillRank = fields[1].GetUInt32();
            g_spellIdToMaxSkillRank[spellId] = maxSkillRank;
        } while (result->NextRow());
    }

    class AccountMountsWorldScript : public WorldScript
    {
    public:
        AccountMountsWorldScript()
            : WorldScript("AccountMountsWorld", {
                WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
                WORLDHOOK_ON_AFTER_CONFIG_LOAD
            })
        {
        }

        void OnBeforeWorldInitialized() override
        {
            LoadConfig();
            sLog.outString("[mod-account-mounts] module loaded.");
        }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            LoadConfig();
        }

    private:
        void LoadConfig()
        {
            g_enabled = sConfig.GetBoolDefault("Account.Mounts.Enable", true);
            g_announce = sConfig.GetBoolDefault("Account.Mounts.Announce", false);
            g_limitRace = sConfig.GetBoolDefault("Account.Mounts.LimitRace", true);
            g_limitClass = sConfig.GetBoolDefault("Account.Mounts.LimitClass", true);
            g_allowAllMounts = sConfig.GetBoolDefault("Account.Mounts.AllowAllMounts", false);

            std::string excludedSpellsStr = sConfig.GetStringDefault("Account.Mounts.ExcludedSpellIDs", "");
            LoadSpellIdSet(excludedSpellsStr, g_excludedSpellIds);

            std::string epicSpellsStr = sConfig.GetStringDefault("Account.Mounts.EpicSpellIDs", "");
            LoadSpellIdSet(epicSpellsStr, g_forcedEpicSpellIds);

            std::string normalSpellsStr = sConfig.GetStringDefault("Account.Mounts.NormalSpellIDs", "");
            LoadSpellIdSet(normalSpellsStr, g_forcedNormalSpellIds);

            LoadMountItemInfo();
        }
    };

    class AccountMountsPlayerScript : public PlayerScript
    {
    public:
        AccountMountsPlayerScript()
            : PlayerScript("AccountMounts", {
                PLAYERHOOK_ON_LOGIN,
                PLAYERHOOK_ON_LEARN_SPELL
            })
        {
        }

        void OnLogin(Player* player) override
        {
            ShareAccountMounts(player);
        }

        void OnLearnSpell(Player* player, uint32 spellId) override
        {
            SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellId);
            if (IsMountSpell(spellInfo) || IsRidingSkillSpell(spellId, spellInfo))
                ShareAccountMounts(player);
        }
    };
}

void AddAccountMountsScripts()
{
    new AccountMountsWorldScript();
    new AccountMountsPlayerScript();
}
