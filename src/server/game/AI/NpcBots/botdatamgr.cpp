#include "bot_ai.h"
#include "botdatamgr.h"
#include "botmgr.h"
#include "botspell.h"
#include "botwanderful.h"
#include "bpet_ai.h"
#include "Containers.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GroupMgr.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectMgr.h"
#include "StringConvert.h"
#include "WorldDatabase.h"
/*
Npc Bot Data Manager by Trickerer (onlysuffering@gmail.com)
NpcBots DB Data management
%Complete: ???
*/

#ifdef _MSC_VER
# pragma warning(push, 4)
#endif

typedef std::unordered_map<uint32 /*entry*/, NpcBotData*> NpcBotDataMap;
typedef std::unordered_map<uint32 /*entry*/, NpcBotAppearanceData*> NpcBotAppearanceDataMap;
typedef std::unordered_map<uint32 /*entry*/, NpcBotExtras*> NpcBotExtrasMap;
typedef std::unordered_map<uint32 /*entry*/, NpcBotTransmogData*> NpcBotTransmogDataMap;
NpcBotDataMap _botsData;
NpcBotAppearanceDataMap _botsAppearanceData;
NpcBotExtrasMap _botsExtras;
NpcBotTransmogDataMap _botsTransmogData;
NpcBotRegistry _existingBots;

CreatureTemplateContainer _botsWanderCreatureTemplates;
std::unordered_map<uint32, EquipmentInfo const*> _botsWanderCreatureEquipmentTemplates;
std::list<std::pair<uint32, WanderNode const*>> _botsWanderCreaturesToSpawn;

constexpr uint8 ITEM_SORTING_LEVEL_STEP = 5;
typedef std::vector<uint32> ItemIdVector;
typedef std::array<ItemIdVector, DEFAULT_MAX_LEVEL / ITEM_SORTING_LEVEL_STEP + 1> ItemLeveledArr;
typedef std::array<ItemLeveledArr, BOT_INVENTORY_SIZE> ItemPerSlot;
typedef std::array<ItemPerSlot, BOT_CLASS_END> ItemPerBotClassMap;
ItemPerBotClassMap _botsWanderCreaturesSortedGear;

constexpr uint32 FLAGS_ONLY_A = AsUnderlyingType(BotWPFlags::BOTWP_FLAG_ALLIANCE_ONLY);
constexpr uint32 FLAGS_ONLY_H = AsUnderlyingType(BotWPFlags::BOTWP_FLAG_HORDE_ONLY);
constexpr uint32 FLAGS_ONLY_A_OR_H = FLAGS_ONLY_A | FLAGS_ONLY_H;

static bool allBotsLoaded = false;

static uint32 next_wandering_bot_spawn_delay = 0;

void BotDataMgr::Update(uint32 diff)
{
    if (_botsWanderCreaturesToSpawn.empty())
        return;

    static const uint32 WANDERING_BOT_SPAWN_DELAY = 250;

    if (next_wandering_bot_spawn_delay >= diff)
    {
        next_wandering_bot_spawn_delay -= diff;
        return;
    }
    else
        next_wandering_bot_spawn_delay += WANDERING_BOT_SPAWN_DELAY;

    auto const& p = _botsWanderCreaturesToSpawn.front();

    uint32 bot_id = p.first;
    WanderNode const* spawnLoc = p.second;

    _botsWanderCreaturesToSpawn.pop_front();

    CreatureTemplate const& bot_template = _botsWanderCreatureTemplates.at(bot_id);
    NpcBotData const* bot_data = SelectNpcBotData(bot_id);
    NpcBotExtras const* bot_extras = SelectNpcBotExtras(bot_id);
    Position spawnPos = spawnLoc->GetPosition();

    ASSERT(bot_data);
    ASSERT(bot_extras);

    Map* map = sMapMgr->CreateBaseMap(spawnLoc->GetMapId());
    map->LoadGrid(spawnLoc->m_positionX, spawnLoc->m_positionY);

    TC_LOG_INFO("npcbots", "Spawning wandering bot: %s (%u) class %u race %u fac %u, location: mapId %u %s (%s)",
        bot_template.Name.c_str(), bot_id, uint32(bot_extras->bclass), uint32(bot_extras->race), bot_data->faction,
        spawnLoc->GetMapId(), spawnLoc->ToString().c_str(), spawnLoc->GetName().c_str());

    Creature* bot = new Creature();
    if (!bot->Create(map->GenerateLowGuid<HighGuid::Unit>(), map, PHASEMASK_NORMAL, bot_id, *spawnLoc))
    {
        delete bot;
        TC_LOG_FATAL("server.loading", "Creature is not created!");
        ASSERT(false);
    }
    if (!bot->LoadBotCreatureFromDB(0, map, true, true, bot_id, &spawnPos))
    {
        delete bot;
        TC_LOG_FATAL("server.loading", "Cannot load npcbot from DB!");
        ASSERT(false);
    }
}

std::shared_mutex* BotDataMgr::GetLock()
{
    static std::shared_mutex _lock;
    return &_lock;
}

bool BotDataMgr::AllBotsLoaded()
{
    return allBotsLoaded;
}

void BotDataMgr::LoadNpcBots(bool spawn)
{
    if (allBotsLoaded)
        return;

    TC_LOG_INFO("server.loading", "Starting NpcBot system...");

    GenerateBotCustomSpells();

    uint32 botoldMSTime = getMSTime();

    Field* field;
    uint8 index;

    //                                                      1       2     3     4     5          6
    QueryResult result = WorldDatabase.Query("SELECT entry, gender, skin, face, hair, haircolor, features FROM creature_template_npcbot_appearance");
    if (result)
    {
        do
        {
            field = result->Fetch();
            index = 0;
            uint32 entry = field[  index].GetUInt32();

            NpcBotAppearanceData* appearanceData = new NpcBotAppearanceData();
            appearanceData->gender =    field[++index].GetUInt8();
            appearanceData->skin =      field[++index].GetUInt8();
            appearanceData->face =      field[++index].GetUInt8();
            appearanceData->hair =      field[++index].GetUInt8();
            appearanceData->haircolor = field[++index].GetUInt8();
            appearanceData->features =  field[++index].GetUInt8();

            _botsAppearanceData[entry] = appearanceData;

        } while (result->NextRow());

        TC_LOG_INFO("server.loading", ">> Bot appearance data loaded");
    }
    else
        TC_LOG_INFO("server.loading", ">> Bots appearance data is not loaded. Table `creature_template_npcbot_appearance` is empty!");

    //                                          1      2
    result = WorldDatabase.Query("SELECT entry, class, race FROM creature_template_npcbot_extras");
    if (result)
    {
        do
        {
            field = result->Fetch();
            index = 0;
            uint32 entry =      field[  index].GetUInt32();

            NpcBotExtras* extras = new NpcBotExtras();
            extras->bclass =    field[++index].GetUInt8();
            extras->race =      field[++index].GetUInt8();

            _botsExtras[entry] = extras;

        } while (result->NextRow());

        TC_LOG_INFO("server.loading", ">> Bot race data loaded");
    }
    else
        TC_LOG_INFO("server.loading", ">> Bots race data is not loaded. Table `creature_template_npcbot_extras` is empty!");

    //                                              1     2        3
    result = CharacterDatabase.Query("SELECT entry, slot, item_id, fake_id FROM characters_npcbot_transmog");
    if (result)
    {
        do
        {
            field = result->Fetch();
            index = 0;
            uint32 entry =          field[  index].GetUInt32();

            if (_botsTransmogData.count(entry) == 0)
                _botsTransmogData[entry] = new NpcBotTransmogData();

            //load data
            uint8 slot =            field[++index].GetUInt8();
            uint32 item_id =        field[++index].GetUInt32();
            uint32 fake_id =        field[++index].GetUInt32();

            _botsTransmogData[entry]->transmogs[slot] = { item_id, fake_id };

        } while (result->NextRow());

        TC_LOG_INFO("server.loading", ">> Bot transmog data loaded");
    }
    else
        TC_LOG_INFO("server.loading", ">> Bots transmog data is not loaded. Table `characters_npcbot_transmog` is empty!");

    //                                       0      1      2      3     4        5          6          7          8          9               10          11          12         13
    result = CharacterDatabase.Query("SELECT entry, owner, roles, spec, faction, equipMhEx, equipOhEx, equipRhEx, equipHead, equipShoulders, equipChest, equipWaist, equipLegs, equipFeet,"
    //   14          15          16         17         18            19            20             21             22         23
        "equipWrist, equipHands, equipBack, equipBody, equipFinger1, equipFinger2, equipTrinket1, equipTrinket2, equipNeck, spells_disabled FROM characters_npcbot");

    if (result)
    {
        uint32 botcounter = 0;
        uint32 datacounter = 0;
        std::set<uint32> botgrids;
        QueryResult infores;
        CreatureTemplate const* proto;
        NpcBotData* botData;
        std::list<uint32> entryList;

        do
        {
            field = result->Fetch();
            index = 0;
            uint32 entry =          field[  index].GetUInt32();

            //load data
            botData = new NpcBotData(0, 0);
            botData->owner =        field[++index].GetUInt32();
            botData->roles =        field[++index].GetUInt32();
            botData->spec =         field[++index].GetUInt8();
            botData->faction =      field[++index].GetUInt32();

            for (uint8 i = BOT_SLOT_MAINHAND; i != BOT_INVENTORY_SIZE; ++i)
                botData->equips[i] = field[++index].GetUInt32();

            if (char const* disabled_spells_str = field[++index].GetCString())
            {
                std::vector<std::string_view> tok = Trinity::Tokenize(disabled_spells_str, ' ', false);
                for (std::vector<std::string_view>::size_type i = 0; i != tok.size(); ++i)
                    botData->disabled_spells.insert(*(Trinity::StringTo<uint32>(tok[i])));
            }

            entryList.push_back(entry);
            _botsData[entry] = botData;
            ++datacounter;

        } while (result->NextRow());

        TC_LOG_INFO("server.loading", ">> Loaded %u bot data entries", datacounter);

        if (spawn)
        {
            for (std::list<uint32>::const_iterator itr = entryList.cbegin(); itr != entryList.cend(); ++itr)
            {
                uint32 entry = *itr;
                proto = sObjectMgr->GetCreatureTemplate(entry);
                if (!proto)
                {
                    TC_LOG_ERROR("server.loading", "Cannot find creature_template entry for npcbot (id: %u)!", entry);
                    continue;
                }
                //                                     1     2    3           4            5           6
                infores = WorldDatabase.PQuery("SELECT guid, map, position_x, position_y"/*, position_z, orientation*/" FROM creature WHERE id = %u", entry);
                if (!infores)
                {
                    TC_LOG_ERROR("server.loading", "Cannot spawn npcbot %s (id: %u), not found in `creature` table!", proto->Name.c_str(), entry);
                    continue;
                }

                field = infores->Fetch();
                uint32 tableGuid = field[0].GetUInt32();
                uint32 mapId = uint32(field[1].GetUInt16());
                float pos_x = field[2].GetFloat();
                float pos_y = field[3].GetFloat();
                //float pos_z = field[4].GetFloat();
                //float ori = field[5].GetFloat();

                CellCoord c = Trinity::ComputeCellCoord(pos_x, pos_y);
                GridCoord g = Trinity::ComputeGridCoord(pos_x, pos_y);
                ASSERT(c.IsCoordValid(), "Invalid Cell coord!");
                ASSERT(g.IsCoordValid(), "Invalid Grid coord!");
                Map* map = sMapMgr->CreateBaseMap(mapId);
                map->LoadGrid(pos_x, pos_y);

                ObjectGuid Guid(HighGuid::Unit, entry, tableGuid);
                TC_LOG_DEBUG("server.loading", "bot %u: spawnId %u, full %s", entry, tableGuid, Guid.ToString().c_str());
                Creature* bot = map->GetCreature(Guid);
                if (!bot) //not in map, use storage
                {
                    //TC_LOG_DEBUG("server.loading", "bot %u: spawnId %u, is not in map on load", entry, tableGuid);
                    typedef Map::CreatureBySpawnIdContainer::const_iterator SpawnIter;
                    std::pair<SpawnIter, SpawnIter> creBounds = map->GetCreatureBySpawnIdStore().equal_range(tableGuid);
                    if (creBounds.first == creBounds.second)
                    {
                        TC_LOG_ERROR("server.loading", "bot %u is not in spawns list, consider re-spawning it!", entry);
                        continue;
                    }
                    bot = creBounds.first->second;
                }
                ASSERT(bot);
                if (!bot->FindMap())
                    TC_LOG_ERROR("server.loading", "bot %u is not in map!", entry);
                if (!bot->IsInWorld())
                    TC_LOG_ERROR("server.loading", "bot %u is not in world!", entry);
                if (!bot->IsAlive())
                {
                    TC_LOG_ERROR("server.loading", "bot %u is dead, respawning!", entry);
                    bot->Respawn();
                }

                TC_LOG_DEBUG("server.loading", ">> Spawned npcbot %s (id: %u, map: %u, grid: %u, cell: %u)", proto->Name.c_str(), entry, mapId, g.GetId(), c.GetId());
                botgrids.insert(g.GetId());
                ++botcounter;
            }

            TC_LOG_INFO("server.loading", ">> Spawned %u npcbot(s) within %u grid(s) in %u ms", botcounter, uint32(botgrids.size()), GetMSTimeDiffToNow(botoldMSTime));
        }
    }
    else
        TC_LOG_INFO("server.loading", ">> Loaded 0 npcbots. Table `characters_npcbot` is empty!");

    allBotsLoaded = true;

    GenerateWanderingBots();
}

void BotDataMgr::LoadNpcBotGroupData()
{
    TC_LOG_INFO("server.loading", "Loading NPCBot Group members...");

    uint32 oldMSTime = getMSTime();

    CharacterDatabase.DirectExecute("DELETE FROM characters_npcbot_group_member WHERE guid NOT IN (SELECT guid FROM `groups`)");
    CharacterDatabase.DirectExecute("DELETE FROM characters_npcbot_group_member WHERE entry NOT IN (SELECT entry FROM characters_npcbot)");

    //                                                   0     1      2            3         4
    QueryResult result = CharacterDatabase.Query("SELECT guid, entry, memberFlags, subgroup, roles FROM characters_npcbot_group_member ORDER BY guid");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 NPCBot group members. DB table `characters_npcbot_group_member` is empty!");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        uint32 creature_id = fields[1].GetUInt32();
        if (!SelectNpcBotExtras(creature_id))
        {
            TC_LOG_WARN("server.loading", "Table `characters_npcbot_group_member` contains non-NPCBot creature %u which will not be loaded!", creature_id);
            continue;
        }

        if (Group* group = sGroupMgr->GetGroupByDbStoreId(fields[0].GetUInt32()))
            group->LoadCreatureMemberFromDB(creature_id, fields[2].GetUInt8(), fields[3].GetUInt8(), fields[4].GetUInt8());
        else
            TC_LOG_ERROR("misc", "BotDataMgr::LoadNpcBotGroupData: Consistency failed, can't find group (storage id: %u)", fields[0].GetUInt32());

        ++count;

    } while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded %u NPCBot group members in %u ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void BotDataMgr::LoadWanderMap(bool reload)
{
    if (WanderNode::GetAllWPsCount() > 0u)
    {
        if (!reload)
            return;

        WanderNode::RemoveAllWPs();
    }

    uint32 botoldMSTime = getMSTime();

    TC_LOG_INFO("server.loading", "Setting up wander map...");

    //                                             0    1   2 3 4 5   6      7       8        9      10   11    12
    QueryResult wres = WorldDatabase.Query("SELECT id,mapid,x,y,z,o,zoneId,areaId,minlevel,maxlevel,flags,name,links FROM creature_template_npcbot_wander_nodes ORDER BY mapid,id");
    if (!wres)
    {
        TC_LOG_FATAL("server.loading", "Failed to load wander points: table `creature_template_npcbot_wander_nodes` is empty!");
        ASSERT(false);
    }

    bool spawn_node_exists = false;
    std::unordered_map<uint32, std::pair<WanderNode*, std::vector<std::pair<std::string, std::string>>>> links_to_create;
    do
    {
        Field* fields = wres->Fetch();
        uint32 index = 0;

        uint32 id             = fields[  index].GetUInt32();
        uint32 mapId          = fields[++index].GetUInt16();
        float x               = fields[++index].GetFloat();
        float y               = fields[++index].GetFloat();
        float z               = fields[++index].GetFloat();
        float o               = fields[++index].GetFloat();
        uint32 zoneId         = fields[++index].GetUInt32();
        uint32 areaId         = fields[++index].GetUInt32();
        uint8 minLevel        = fields[++index].GetUInt8();
        uint8 maxLevel        = fields[++index].GetUInt8();
        uint32 flags          = fields[++index].GetUInt32();
        std::string name      = fields[++index].GetString();
        std::string_view lstr = fields[++index].GetStringView();

        WanderNode::nextWPId = std::max<uint32>(WanderNode::nextWPId, id);
        spawn_node_exists |= !!(flags & AsUnderlyingType(BotWPFlags::BOTWP_FLAG_SPAWN));

        if (minLevel == 1u && maxLevel == DEFAULT_MAX_LEVEL)
            TC_LOG_WARN("server.loading", "WP %u has no levels set.", id);

        if (!minLevel || !maxLevel || minLevel > DEFAULT_MAX_LEVEL || maxLevel > DEFAULT_MAX_LEVEL || minLevel > maxLevel)
        {
            TC_LOG_WARN("server.loading", "WP %u has invalid levels min %u max %u! Setting to default...",
                id, uint32(minLevel), uint32(maxLevel));
            minLevel = 1;
            maxLevel = DEFAULT_MAX_LEVEL;
        }

        if (flags >= AsUnderlyingType(BotWPFlags::BOTWP_FLAG_END))
        {
            TC_LOG_WARN("server.loading", "WP %u has invalid flags %u! Removing all invalid flags...", id, flags);
            flags &= (AsUnderlyingType(BotWPFlags::BOTWP_FLAG_END) - 1);
        }

        const uint32 conflicting_flags_1 = AsUnderlyingType(BotWPFlags::BOTWP_FLAG_ALLIANCE_ONLY) | AsUnderlyingType(BotWPFlags::BOTWP_FLAG_HORDE_ONLY);
        if ((flags & conflicting_flags_1) == conflicting_flags_1)
        {
            TC_LOG_WARN("server.loading", "WP %u has conflicting flags %u+%u! Removing both...",
                id, AsUnderlyingType(BotWPFlags::BOTWP_FLAG_ALLIANCE_ONLY), AsUnderlyingType(BotWPFlags::BOTWP_FLAG_HORDE_ONLY));
            flags &= ~conflicting_flags_1;
        }

        WanderNode* wp = new WanderNode(id, mapId, x, y, z, o, zoneId, areaId, name);
        wp->SetLevels(minLevel, maxLevel);
        wp->SetFlags(BotWPFlags(flags));

        if (lstr.empty())
        {
            TC_LOG_WARN("server.loading", "WP %u has no links!", id);
            continue;
        }
        std::vector<std::string_view> tok = Trinity::Tokenize(lstr, ' ', false);
        for (std::vector<std::string_view>::size_type i = 0; i != tok.size(); ++i)
        {
            std::vector<std::string_view> link_str = Trinity::Tokenize(tok[i], ':', false);
            ASSERT(link_str.size() == 2u, "Invalid links_str format: '%s'", std::string(tok[i].data(), tok[i].length()).c_str());
            ASSERT(link_str[0].find(" ") == std::string_view::npos);
            ASSERT(link_str[1].find(" ") == std::string_view::npos);
            ASSERT(Trinity::StringTo<uint32>(link_str[0]) != std::nullopt, "Invalid links_str format: '%s'", std::string(tok[i].data(), tok[i].length()).c_str());
            ASSERT(Trinity::StringTo<uint32>(link_str[1]) != std::nullopt, "Invalid links_str format: '%s'", std::string(tok[i].data(), tok[i].length()).c_str());

            std::pair<std::string, std::string> tok_pair = { std::string(link_str[0].data(), link_str[0].length()), std::string(link_str[1].data(), link_str[1].length()) };
            if (links_to_create.find(id) == links_to_create.cend())
                links_to_create[id] = { wp, {std::move(tok_pair)} };
            else
                links_to_create.at(id).second.push_back(std::move(tok_pair));
        }

    } while (wres->NextRow());

    if (!spawn_node_exists)
    {
        TC_LOG_FATAL("server.loading", "Not a single spawn node exists! Spawning wandering bots is impossible! Aborting.");
        ASSERT(false);
    }

    uint32 total_ribs = 0;
    float mindist = 50000.f;
    float maxdist = 0.f;
    for (auto const& vt : links_to_create)
    {
        for (auto const& p : vt.second.second)
        {
            uint32 lid = *Trinity::StringTo<uint32>(p.first);
            if (lid == vt.first)
            {
                TC_LOG_ERROR("server.loading", "WP %u has link %u which links to itself! Skipped.", vt.first, lid);
                continue;
            }

            WanderNode* lwp = WanderNode::FindInAllWPs(lid);
            if (!lwp)
            {
                TC_LOG_ERROR("server.loading", "WP %u has link %u which does not exist!", vt.first, lid);
                continue;
            }
            if (lwp->GetMapId() != vt.second.first->GetMapId())
            {
                TC_LOG_ERROR("server.loading", "WP %u map %u has link %u ON A DIFFERENT MAP %u!", vt.first, vt.second.first->GetMapId(), lid, lwp->GetMapId());
                continue;
            }
            float lwpdist2d = vt.second.first->GetExactDist2d(lwp);
            if (lwpdist2d > MAX_WANDER_NODE_DISTANCE)
                TC_LOG_WARN("server.loading", "Warning! Link distance between WP %u and %u is too great (%.2f)", vt.first, lid, lwpdist2d);
            if (lwpdist2d < MIN_WANDER_NODE_DISTANCE)
                TC_LOG_WARN("server.loading", "Warning! Link distance between WP %u and %u is low (%.2f)", vt.first, lid, lwpdist2d);

            if (!vt.second.first->HasLink(lwp))
            {
                ++total_ribs;
                vt.second.first->Link(lwp);
                float dist2d = vt.second.first->GetExactDist2d(lwp);
                if (dist2d < mindist)
                    mindist = dist2d;
                if (dist2d > maxdist)
                    maxdist = dist2d;
            }
        }
    }

    std::set<WanderNode const*> tops;
    WanderNode::DoForAllWPs([&](WanderNode const* wp) {
        if (tops.count(wp) == 0u && wp->GetLinks().size() == 1u)
        {
            TC_LOG_TRACE("server.loading", "Node %u ('%s') has single connection!", wp->GetWPId(), wp->GetName().c_str());
            WanderNode const* tn = wp->GetLinks().front();
            std::vector<WanderNode const*> sc_chain;
            sc_chain.push_back(wp);
            while (tn != wp)
            {
                if (tn->GetLinks().size() != 2u)
                {
                    sc_chain.push_back(tn);
                    break;
                }
                uint32 prevId = sc_chain.back()->GetWPId();
                sc_chain.push_back(tn);
                tn = *std::find_if_not(std::cbegin(tn->GetLinks()), std::cend(tn->GetLinks()), [nId = prevId](WanderNode const* lwp) {
                    return lwp->GetWPId() == nId;
                });
            }
            if (sc_chain.back()->GetLinks().size() == 1u)
            {
                TC_LOG_TRACE("server.loading", "Node %u ('%s') has single connection!", tn->GetWPId(), tn->GetName().c_str());
                tops.emplace(sc_chain.front());
                tops.emplace(sc_chain.back());
                std::ostringstream ss;
                ss << "Node " << (sc_chain.size() == 2u ? "pair " : "chain ");
                for (uint32 i = 0u; i < sc_chain.size(); ++i)
                {
                    ss << sc_chain[i]->GetWPId();
                    if (i < sc_chain.size() - 1u)
                        ss << '-';
                }
                ss << " is isolated!";
                TC_LOG_INFO("server.loading", ss.str().c_str());
            }
        }
    });

    TC_LOG_INFO("server.loading", ">> Loaded %u bot wander nodes on %u maps (total %u ribs, %u tops) in %u ms. Distances: min %.3f, max %.3f",
        uint32(WanderNode::GetAllWPsCount()), uint32(WanderNode::GetWPMapsCount()), total_ribs, uint32(tops.size()),
        GetMSTimeDiffToNow(botoldMSTime), mindist, maxdist);
}

void BotDataMgr::GenerateWanderingBots()
{
    using NodeVec = std::vector<WanderNode const*>;

    const std::map<uint8, uint32> wbot_faction_for_ex_class = {
        {BOT_CLASS_BM, 2u},
        {BOT_CLASS_SPHYNX, 14u},
        {BOT_CLASS_ARCHMAGE, 1u},
        {BOT_CLASS_DREADLORD, 14u},
        {BOT_CLASS_SPELLBREAKER, 1610u},
        {BOT_CLASS_DARK_RANGER, 14u},
        {BOT_CLASS_NECROMANCER, 14u},
        {BOT_CLASS_SEA_WITCH, 14u}
    };

    /// @TODO: manage allowed world maps HERE: 0, 1 530, 571
    const std::array wbot_allowed_maps{ 0u };

    const uint32 wandering_bots_desired = BotMgr::GetDesiredWanderingBotsCount();

    if (wandering_bots_desired == 0)
        return;

    LoadWanderMap();
    CreateWanderingBotsSortedGear();

    TC_LOG_INFO("server.loading", "Spawning wandering bots...");

    uint32 oldMSTime = getMSTime();

    std::map<uint8, std::set<uint32>> spareBotIdsPerClassMap;
    for (uint8 c = BOT_CLASS_WARRIOR; c < BOT_CLASS_END; ++c)
        if (c != BOT_CLASS_BM && BotMgr::IsClassEnabled(c) && spareBotIdsPerClassMap.find(c) == spareBotIdsPerClassMap.cend())
            spareBotIdsPerClassMap.insert({ c, {} });

    uint32 maxWanderingBots = 0;
    uint32 enabledBotsCount = 0;
    for (decltype(_botsExtras)::value_type const& vt : _botsExtras)
    {
        uint8 c = vt.second->bclass;
        if (c != BOT_CLASS_NONE && c != BOT_CLASS_BM && BotMgr::IsClassEnabled(c))
        {
            ++enabledBotsCount;
            if (_botsData.find(vt.first) == _botsData.end())
            {
                ASSERT(spareBotIdsPerClassMap.find(c) != spareBotIdsPerClassMap.cend());
                spareBotIdsPerClassMap.at(c).insert(vt.first);
                ++maxWanderingBots;
            }
        }
    }

    for (uint8 c = BOT_CLASS_WARRIOR; c < BOT_CLASS_END; ++c)
        if (spareBotIdsPerClassMap.find(c) != spareBotIdsPerClassMap.cend() && spareBotIdsPerClassMap.at(c).empty())
            spareBotIdsPerClassMap.erase(c);

    if (maxWanderingBots < wandering_bots_desired)
    {
        TC_LOG_FATAL("server.loading", "Only %u out of %u bots of enabled classes aren't spawned. Desired amount of wandering bots (%u) cannot be created. Aborting!",
            maxWanderingBots, enabledBotsCount, wandering_bots_desired);
        ASSERT(false);
    }

    uint32 bot_id = BOT_ENTRY_CREATE_BEGIN - 1;
    QueryResult result = CharacterDatabase.PQuery("SELECT value FROM worldstates WHERE entry = %u", uint32(BOT_GIVER_ENTRY));
    if (!result)
    {
        TC_LOG_WARN("server.loading", "Next bot id for autogeneration is not found! Resetting! (client cache may interfere with names)");
        for (uint32 bot_cid : GetExistingNPCBotIds())
            if (bot_cid > bot_id)
                bot_id = bot_cid;
        CharacterDatabase.DirectPExecute("INSERT INTO worldstates (entry, value, comment) VALUES (%u, %u, '%s')",
            uint32(BOT_GIVER_ENTRY), bot_id, "NPCBOTS MOD - last autogenerated bot entry");
    }
    else
        bot_id = result->Fetch()[0].GetUInt32();

    ASSERT(bot_id > BOT_ENTRY_BEGIN);

    CreatureTemplateContainer const& all_templates = sObjectMgr->GetCreatureTemplates();

    auto find_bot_creature_template_by_botclass = [&spareBotIdsPerClassMap](uint8 b_class) -> CreatureTemplate const* {
        ASSERT(spareBotIdsPerClassMap.find(b_class) != spareBotIdsPerClassMap.cend());
        auto const& cSet = spareBotIdsPerClassMap.at(b_class);
        uint32 entry = cSet.size() == 1 ? *cSet.cbegin() : Trinity::Containers::SelectRandomContainerElement(cSet);
        return sObjectMgr->GetCreatureTemplate(entry);
    };

    auto remove_bot_orig_entry_from_available = [&spareBotIdsPerClassMap](uint8 bclass, uint32 botId) {
        ASSERT(spareBotIdsPerClassMap.find(bclass) != spareBotIdsPerClassMap.cend());
        ASSERT(spareBotIdsPerClassMap.at(bclass).find(botId) != spareBotIdsPerClassMap.at(bclass).cend());
        spareBotIdsPerClassMap.at(bclass).erase(botId);
        if (spareBotIdsPerClassMap.at(bclass).empty())
            spareBotIdsPerClassMap.erase(bclass);
    };

    NodeVec spawns_a, spawns_h, spawns_rest;
    for (NodeVec* vec : { &spawns_a, &spawns_h, &spawns_rest })
        vec->reserve(WanderNode::GetWPMapsCount() * 20u);

    WanderNode::DoForAllWPs([&spawns_a, &spawns_h, &spawns_rest, &wbot_allowed_maps](WanderNode const* wp) {
        if (std::find(std::cbegin(wbot_allowed_maps), std::cend(wbot_allowed_maps), wp->GetMapId()) != std::cend(wbot_allowed_maps))
        {
            uint32 flags = wp->GetFlags();
            if (flags & AsUnderlyingType(BotWPFlags::BOTWP_FLAG_SPAWN))
            {
                if (flags & AsUnderlyingType(BotWPFlags::BOTWP_FLAG_ALLIANCE_ONLY))
                    spawns_a.push_back(wp);
                else if (flags & AsUnderlyingType(BotWPFlags::BOTWP_FLAG_HORDE_ONLY))
                    spawns_h.push_back(wp);
                else
                {
                    spawns_a.push_back(wp);
                    spawns_h.push_back(wp);
                    spawns_rest.push_back(wp);
                }
            }
        }
    });

    if (spawns_a.empty() || spawns_h.empty() || spawns_rest.empty())
    {
        TC_LOG_FATAL("server.loading", "Not all factions (a/h/m) have spawn points, make sure at least one exists for each! Aborting!");
        ASSERT(false);
    }

    bool found_maxlevel_node_a = false;
    bool found_maxlevel_node_h = false;
    bool found_maxlevel_node_rest = false;
    const uint8 maxof_minclasslvl_nor = GetMinLevelForBotClass(BOT_CLASS_DEATH_KNIGHT); // 55
    const uint8 maxof_minclasslvl_ex = GetMinLevelForBotClass(BOT_CLASS_DREADLORD); // 60
    for (WanderNode const* wp : spawns_a)
    {
        if (wp->GetLevels().second >= maxof_minclasslvl_nor)
        {
            found_maxlevel_node_a = true;
            break;
        }
    }
    for (WanderNode const* wp : spawns_h)
    {
        if (wp->GetLevels().second >= maxof_minclasslvl_nor)
        {
            found_maxlevel_node_h = true;
            break;
        }
    }
    for (WanderNode const* wp : spawns_rest)
    {
        if (wp->GetLevels().second >= maxof_minclasslvl_ex)
        {
            found_maxlevel_node_rest = true;
            break;
        }
    }
    if (!found_maxlevel_node_a)
    {
        TC_LOG_FATAL("server.loading", "Not a single spawn point exists for Alliance DKs!");
        ASSERT(false);
    }
    if (!found_maxlevel_node_h)
    {
        TC_LOG_FATAL("server.loading", "Not a single spawn point exists for Horde DKs!");
        ASSERT(false);
    }
    if (!found_maxlevel_node_rest)
    {
        TC_LOG_FATAL("server.loading", "Not a single spawn point exists for extra classes (level 60)!");
        ASSERT(false);
    }

    std::set<uint32> botgrids;
    for (uint32 i = 1; i <= wandering_bots_desired; ++i) // i is a counter, NOT used as index or value
    {
        while (all_templates.find(++bot_id) != all_templates.cend()) {}

        uint8 bot_class = Trinity::Containers::SelectRandomContainerElement(spareBotIdsPerClassMap).first;
        CreatureTemplate const* orig_template = find_bot_creature_template_by_botclass(bot_class);
        ASSERT(orig_template);
        NpcBotExtras const* orig_extras = SelectNpcBotExtras(orig_template->Entry);
        ASSERT_NOTNULL(orig_extras);
        ChrRacesEntry const* rentry = sChrRacesStore.LookupEntry(orig_extras->race);
        uint32 bot_faction = (bot_class >= BOT_CLASS_EX_START) ? wbot_faction_for_ex_class.find(bot_class)->second : rentry ? rentry->FactionID : 14;

        NodeVec* bot_spawn_nodes;
        TeamId bot_team = GetTeamForFaction(bot_faction);
        switch (bot_team)
        {
            case TEAM_ALLIANCE:
                bot_spawn_nodes = &spawns_a;
                break;
            case TEAM_HORDE:
                bot_spawn_nodes = &spawns_h;
                break;
            default:
                bot_spawn_nodes = &spawns_rest;
                break;
        }
        NodeVec level_nodes;
        level_nodes.reserve(bot_spawn_nodes->size());
        uint8 myminlevel = GetMinLevelForBotClass(bot_class);
        for (WanderNode const* node : *bot_spawn_nodes)
        {
            if (myminlevel <= node->GetLevels().second)
                level_nodes.push_back(node);
        }

        WanderNode const* spawnLoc = level_nodes.empty() ? nullptr : Trinity::Containers::SelectRandomContainerElement(level_nodes);
        if (!spawnLoc)
        {
            TC_LOG_WARN("server.loading", "No viable nodes found for bot %s id %u orig id %u class %u race %u faction %u team %u. Skipping.",
                orig_template->Name.c_str(), bot_id, orig_template->Entry, uint32(orig_extras->bclass), uint32(orig_extras->race),
                bot_faction, uint32(bot_team));
            --i;
            --bot_id;
            continue;
        }

        CreatureTemplate& bot_template = _botsWanderCreatureTemplates[bot_id];
        //copy all fields
        bot_template = *orig_template;
        bot_template.Entry = bot_id;
        bot_template.Title = "";
        bot_template.InitializeQueryData();

        NpcBotData* bot_data = new NpcBotData(bot_ai::DefaultRolesForClass(bot_class), bot_faction, bot_ai::DefaultSpecForClass(bot_class));
        _botsData[bot_id] = bot_data;
        NpcBotExtras* bot_extras = new NpcBotExtras();
        bot_extras->bclass = bot_class;
        bot_extras->race = orig_extras->race;
        _botsExtras[bot_id] = bot_extras;
        if (NpcBotAppearanceData const* orig_apdata = SelectNpcBotAppearance(orig_template->Entry))
        {
            NpcBotAppearanceData* bot_apdata = new NpcBotAppearanceData();
            bot_apdata->face = orig_apdata->face;
            bot_apdata->features = orig_apdata->features;
            bot_apdata->gender = orig_apdata->gender;
            bot_apdata->hair = orig_apdata->hair;
            bot_apdata->haircolor = orig_apdata->haircolor;
            bot_apdata->skin = orig_apdata->skin;
            _botsAppearanceData[bot_id] = bot_apdata;
        }
        int8 beqId = 1;
        _botsWanderCreatureEquipmentTemplates[bot_id] = sObjectMgr->GetEquipmentInfo(orig_template->Entry, beqId);

        //We do not create CreatureData for generated bots

        CellCoord c = Trinity::ComputeCellCoord(spawnLoc->m_positionX, spawnLoc->m_positionY);
        GridCoord g = Trinity::ComputeGridCoord(spawnLoc->m_positionX, spawnLoc->m_positionY);
        ASSERT(c.IsCoordValid(), "Invalid Cell coord!");
        ASSERT(g.IsCoordValid(), "Invalid Grid coord!");
        Map const* map = sMapMgr->CreateBaseMap(spawnLoc->GetMapId());
        ASSERT(!map->Instanceable(), map->GetDebugInfo().c_str());

        _botsWanderCreaturesToSpawn.push_back({ bot_id, spawnLoc });

        remove_bot_orig_entry_from_available(bot_class, orig_template->Entry);

        botgrids.insert(g.GetId());
    }

    CharacterDatabase.PExecute("UPDATE worldstates SET value = %u WHERE entry = %u", bot_id, uint32(BOT_GIVER_ENTRY));

    TC_LOG_INFO("server.loading", ">> Set up spawning of %u wandering bots in %u grids in %u ms",
        uint32(_botsWanderCreatureTemplates.size()), uint32(botgrids.size()), GetMSTimeDiffToNow(oldMSTime));
}

void BotDataMgr::CreateWanderingBotsSortedGear()
{
    //uint32 botoldMSTime = getMSTime();

    TC_LOG_INFO("server.loading", "Sorting wandering bot's gear...");

    const std::map<uint32, uint8> InvTypeToBotSlot = {
        {INVTYPE_HEAD, BOT_SLOT_HEAD},
        {INVTYPE_SHOULDERS, BOT_SLOT_SHOULDERS},
        {INVTYPE_CHEST, BOT_SLOT_CHEST},
        {INVTYPE_WAIST, BOT_SLOT_WAIST},
        {INVTYPE_LEGS, BOT_SLOT_LEGS},
        {INVTYPE_FEET, BOT_SLOT_FEET},
        {INVTYPE_WRISTS, BOT_SLOT_WRIST},
        {INVTYPE_HANDS, BOT_SLOT_HANDS}
    };

    ItemTemplateContainer const& all_item_templates = sObjectMgr->GetItemTemplateStore();
    for (auto const& kv : all_item_templates)
    {
        ItemTemplate const& proto = kv.second;

        if (proto.ItemLevel == 0)
            continue;

        switch (proto.Quality)
        {
            case ITEM_QUALITY_POOR:
                if (proto.RequiredLevel > 1)
                    continue;
                break;
            case ITEM_QUALITY_NORMAL:
                if (proto.RequiredLevel > 14)
                    continue;
                break;
            case ITEM_QUALITY_UNCOMMON:
            case ITEM_QUALITY_RARE:
            case ITEM_QUALITY_EPIC:
                if (!(proto.RequiredLevel >= 2 && proto.RequiredLevel <= DEFAULT_MAX_LEVEL))
                    continue;
                break;
            default:
                continue;
        }

        uint32 itemId = kv.first;
        uint8 reqLstep = (proto.RequiredLevel + ITEM_SORTING_LEVEL_STEP - 1) / ITEM_SORTING_LEVEL_STEP;

        switch (proto.Class)
        {
            case ITEM_CLASS_ARMOR:
                switch (proto.InventoryType)
                {
                    case INVTYPE_FINGER:
                        if (proto.Quality < ITEM_QUALITY_UNCOMMON)
                            break;
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_FINGER1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PALADIN][BOT_SLOT_FINGER1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_FINGER1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_FINGER1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PRIEST][BOT_SLOT_FINGER1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DEATH_KNIGHT][BOT_SLOT_FINGER1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_FINGER1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_MAGE][BOT_SLOT_FINGER1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARLOCK][BOT_SLOT_FINGER1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DRUID][BOT_SLOT_FINGER1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_BM][BOT_SLOT_FINGER1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_ARCHMAGE][BOT_SLOT_FINGER1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DREADLORD][BOT_SLOT_FINGER1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SPELLBREAKER][BOT_SLOT_FINGER1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DARK_RANGER][BOT_SLOT_FINGER1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_NECROMANCER][BOT_SLOT_FINGER1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SEA_WITCH][BOT_SLOT_FINGER1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_FINGER2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PALADIN][BOT_SLOT_FINGER2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_FINGER2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_FINGER2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PRIEST][BOT_SLOT_FINGER2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DEATH_KNIGHT][BOT_SLOT_FINGER2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_FINGER2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_MAGE][BOT_SLOT_FINGER2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARLOCK][BOT_SLOT_FINGER2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DRUID][BOT_SLOT_FINGER2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_BM][BOT_SLOT_FINGER2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_ARCHMAGE][BOT_SLOT_FINGER2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DREADLORD][BOT_SLOT_FINGER2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SPELLBREAKER][BOT_SLOT_FINGER2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DARK_RANGER][BOT_SLOT_FINGER2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_NECROMANCER][BOT_SLOT_FINGER2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SEA_WITCH][BOT_SLOT_FINGER2][reqLstep].push_back(itemId);
                        break;
                    case INVTYPE_TRINKET:
                        if (proto.Quality < ITEM_QUALITY_UNCOMMON)
                            break;
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_TRINKET1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PALADIN][BOT_SLOT_TRINKET1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_TRINKET1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_TRINKET1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PRIEST][BOT_SLOT_TRINKET1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DEATH_KNIGHT][BOT_SLOT_TRINKET1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_TRINKET1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_MAGE][BOT_SLOT_TRINKET1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARLOCK][BOT_SLOT_TRINKET1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DRUID][BOT_SLOT_TRINKET1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_BM][BOT_SLOT_TRINKET1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_ARCHMAGE][BOT_SLOT_TRINKET1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DREADLORD][BOT_SLOT_TRINKET1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SPELLBREAKER][BOT_SLOT_TRINKET1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DARK_RANGER][BOT_SLOT_TRINKET1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_NECROMANCER][BOT_SLOT_TRINKET1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SEA_WITCH][BOT_SLOT_TRINKET1][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_TRINKET2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PALADIN][BOT_SLOT_TRINKET2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_TRINKET2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_TRINKET2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PRIEST][BOT_SLOT_TRINKET2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DEATH_KNIGHT][BOT_SLOT_TRINKET2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_TRINKET2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_MAGE][BOT_SLOT_TRINKET2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARLOCK][BOT_SLOT_TRINKET2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DRUID][BOT_SLOT_TRINKET2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_BM][BOT_SLOT_TRINKET2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_ARCHMAGE][BOT_SLOT_TRINKET2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DREADLORD][BOT_SLOT_TRINKET2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SPELLBREAKER][BOT_SLOT_TRINKET2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DARK_RANGER][BOT_SLOT_TRINKET2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_NECROMANCER][BOT_SLOT_TRINKET2][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SEA_WITCH][BOT_SLOT_TRINKET2][reqLstep].push_back(itemId);
                        break;
                    case INVTYPE_CLOAK:
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_BACK][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PALADIN][BOT_SLOT_BACK][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_BACK][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_BACK][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PRIEST][BOT_SLOT_BACK][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DEATH_KNIGHT][BOT_SLOT_BACK][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_BACK][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_MAGE][BOT_SLOT_BACK][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARLOCK][BOT_SLOT_BACK][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DRUID][BOT_SLOT_BACK][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_BM][BOT_SLOT_BACK][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_ARCHMAGE][BOT_SLOT_BACK][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DREADLORD][BOT_SLOT_BACK][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SPELLBREAKER][BOT_SLOT_BACK][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DARK_RANGER][BOT_SLOT_BACK][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_NECROMANCER][BOT_SLOT_BACK][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SEA_WITCH][BOT_SLOT_BACK][reqLstep].push_back(itemId);
                        break;
                    case INVTYPE_HOLDABLE:
                        if (proto.Quality < ITEM_QUALITY_UNCOMMON)
                            break;
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PRIEST][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_MAGE][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARLOCK][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DRUID][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        break;
                    case INVTYPE_SHIELD:
                        if (proto.Armor == 0)
                            break;
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PALADIN][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SPELLBREAKER][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        break;
                    case INVTYPE_HEAD:
                    case INVTYPE_SHOULDERS:
                    case INVTYPE_CHEST:
                    case INVTYPE_WAIST:
                    case INVTYPE_LEGS:
                    case INVTYPE_FEET:
                    case INVTYPE_WRISTS:
                    case INVTYPE_HANDS:
                    {
                        if (proto.Armor == 0)
                            break;
                        decltype(InvTypeToBotSlot)::const_iterator ci = InvTypeToBotSlot.find(proto.InventoryType);
                        ASSERT(ci != InvTypeToBotSlot.cend());
                        uint8 slot = ci->second;
                        switch (proto.SubClass)
                        {
                            case ItemSubclassArmor::ITEM_SUBCLASS_ARMOR_CLOTH:
                                _botsWanderCreaturesSortedGear[BOT_CLASS_PRIEST][slot][reqLstep].push_back(itemId);
                                _botsWanderCreaturesSortedGear[BOT_CLASS_MAGE][slot][reqLstep].push_back(itemId);
                                _botsWanderCreaturesSortedGear[BOT_CLASS_WARLOCK][slot][reqLstep].push_back(itemId);
                                _botsWanderCreaturesSortedGear[BOT_CLASS_ARCHMAGE][slot][reqLstep].push_back(itemId);
                                _botsWanderCreaturesSortedGear[BOT_CLASS_NECROMANCER][slot][reqLstep].push_back(itemId);
                                _botsWanderCreaturesSortedGear[BOT_CLASS_SEA_WITCH][slot][reqLstep].push_back(itemId);
                                break;
                            case ItemSubclassArmor::ITEM_SUBCLASS_ARMOR_LEATHER:
                                _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][slot][reqLstep].push_back(itemId);
                                _botsWanderCreaturesSortedGear[BOT_CLASS_DRUID][slot][reqLstep].push_back(itemId);
                                if (proto.RequiredLevel < 40)
                                {
                                    _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][slot][reqLstep].push_back(itemId);
                                    _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][slot][reqLstep].push_back(itemId);
                                }
                                _botsWanderCreaturesSortedGear[BOT_CLASS_DARK_RANGER][slot][reqLstep].push_back(itemId);
                                break;
                            case ItemSubclassArmor::ITEM_SUBCLASS_ARMOR_MAIL:
                                if (proto.RequiredLevel < 40)
                                {
                                    _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][slot][reqLstep].push_back(itemId);
                                    _botsWanderCreaturesSortedGear[BOT_CLASS_PALADIN][slot][reqLstep].push_back(itemId);

                                }
                                else
                                {
                                    _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][slot][reqLstep].push_back(itemId);
                                    _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][slot][reqLstep].push_back(itemId);
                                }
                                _botsWanderCreaturesSortedGear[BOT_CLASS_BM][slot][reqLstep].push_back(itemId);
                                _botsWanderCreaturesSortedGear[BOT_CLASS_SPHYNX][slot][reqLstep].push_back(itemId);
                                _botsWanderCreaturesSortedGear[BOT_CLASS_SPELLBREAKER][slot][reqLstep].push_back(itemId);
                                break;
                            case ItemSubclassArmor::ITEM_SUBCLASS_ARMOR_PLATE:
                                _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][slot][reqLstep].push_back(itemId);
                                _botsWanderCreaturesSortedGear[BOT_CLASS_PALADIN][slot][reqLstep].push_back(itemId);
                                _botsWanderCreaturesSortedGear[BOT_CLASS_DEATH_KNIGHT][slot][reqLstep].push_back(itemId);
                                _botsWanderCreaturesSortedGear[BOT_CLASS_BM][slot][reqLstep].push_back(itemId);
                                _botsWanderCreaturesSortedGear[BOT_CLASS_SPHYNX][slot][reqLstep].push_back(itemId);
                                _botsWanderCreaturesSortedGear[BOT_CLASS_SPELLBREAKER][slot][reqLstep].push_back(itemId);
                                _botsWanderCreaturesSortedGear[BOT_CLASS_DREADLORD][slot][reqLstep].push_back(itemId);
                                break;
                            default:
                                break;
                        }
                        break;
                    }
                    default:
                        break;
                }
                break;
            case ITEM_CLASS_WEAPON:
                if (proto.Damage[0].DamageMin < 1.0f)
                    break;
                switch (proto.SubClass)
                {
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_WAND:
                        if (proto.InventoryType != INVTYPE_RANGED && proto.InventoryType != INVTYPE_RANGEDRIGHT)
                            break;
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SPHYNX][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SPHYNX][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PRIEST][BOT_SLOT_RANGED][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_MAGE][BOT_SLOT_RANGED][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARLOCK][BOT_SLOT_RANGED][reqLstep].push_back(itemId);
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_GUN:
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_CROSSBOW:
                        if (proto.InventoryType != INVTYPE_RANGED && proto.InventoryType != INVTYPE_RANGEDRIGHT)
                            break;
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_RANGED][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_RANGED][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_RANGED][reqLstep].push_back(itemId);
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_BOW:
                        if (proto.InventoryType != INVTYPE_RANGED && proto.InventoryType != INVTYPE_RANGEDRIGHT)
                            break;
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_RANGED][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_RANGED][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_RANGED][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DARK_RANGER][BOT_SLOT_RANGED][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SEA_WITCH][BOT_SLOT_RANGED][reqLstep].push_back(itemId);
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_THROWN:
                        if (proto.InventoryType != INVTYPE_THROWN)
                            break;
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_RANGED][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_RANGED][reqLstep].push_back(itemId);
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_STAFF:
                        if (proto.InventoryType != INVTYPE_2HWEAPON)
                            break;
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PRIEST][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_MAGE][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARLOCK][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DRUID][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_ARCHMAGE][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_NECROMANCER][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DREADLORD][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_AXE2:
                        if (proto.InventoryType != INVTYPE_2HWEAPON)
                            break;
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.RequiredLevel >= 60 - ITEM_SORTING_LEVEL_STEP)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PALADIN][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DEATH_KNIGHT][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_BM][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DREADLORD][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_SWORD2:
                        if (proto.InventoryType != INVTYPE_2HWEAPON)
                            break;
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.RequiredLevel >= 60 - ITEM_SORTING_LEVEL_STEP)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PALADIN][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DEATH_KNIGHT][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_BM][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DREADLORD][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_POLEARM:
                        if (proto.InventoryType != INVTYPE_2HWEAPON)
                            break;
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.RequiredLevel >= 60 - ITEM_SORTING_LEVEL_STEP)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PALADIN][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DEATH_KNIGHT][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DRUID][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_BM][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DREADLORD][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_MACE2:
                        if (proto.InventoryType != INVTYPE_2HWEAPON)
                            break;
                        _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.RequiredLevel >= 60 - ITEM_SORTING_LEVEL_STEP)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_PALADIN][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DEATH_KNIGHT][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DRUID][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        _botsWanderCreaturesSortedGear[BOT_CLASS_DREADLORD][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_AXE:
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_PALADIN][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_DEATH_KNIGHT][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_DEATH_KNIGHT][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_SPELLBREAKER][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_MACE:
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_PALADIN][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_DEATH_KNIGHT][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_DEATH_KNIGHT][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_PRIEST][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_DRUID][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_SPELLBREAKER][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_SWORD:
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_PALADIN][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_DEATH_KNIGHT][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_DEATH_KNIGHT][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_MAGE][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_WARLOCK][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_SPELLBREAKER][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_DARK_RANGER][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_DARK_RANGER][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_FIST:
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_DRUID][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_SPELLBREAKER][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_DAGGER:
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_WARRIOR][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_HUNTER][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_ROGUE][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_PRIEST][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_MAGE][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_WARLOCK][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_DRUID][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_SHAMAN][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_SPELLBREAKER][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_DARK_RANGER][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_DARK_RANGER][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_SEA_WITCH][BOT_SLOT_MAINHAND][reqLstep].push_back(itemId);
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                            _botsWanderCreaturesSortedGear[BOT_CLASS_SEA_WITCH][BOT_SLOT_OFFHAND][reqLstep].push_back(itemId);
                        break;
                    default:
                        break;
                }
                break;
            default:
                break;
        }
    }

    TC_LOG_INFO("server.loading", ">> Done");
}

Item* BotDataMgr::GenerateWanderingBotItem(uint8 slot, uint8 botclass, uint8 level, std::function<bool(ItemTemplate const*)>&& check)
{
    ASSERT(slot < BOT_INVENTORY_SIZE);
    ASSERT(botclass < BOT_CLASS_END);
    ASSERT(level <= DEFAULT_MAX_LEVEL + 4);

    ItemIdVector const& itemIdVec = _botsWanderCreaturesSortedGear[botclass][slot][level / ITEM_SORTING_LEVEL_STEP];

    if (!itemIdVec.empty())
    {
        uint32 itemId;
        uint8 tries = 0;
        bool can_equip = false;
        do
        {
            ++tries;
            itemId = Trinity::Containers::SelectRandomContainerElement(itemIdVec);
            can_equip = check(sObjectMgr->GetItemTemplate(itemId));

        } while (!can_equip && tries < 20);

        if (can_equip)
        {
            if (Item* newItem = Item::CreateItem(itemId, 1, nullptr))
            {
                if (uint32 randomPropertyId = GenerateItemRandomPropertyId(itemId))
                    newItem->SetItemRandomProperties(randomPropertyId);

                return newItem;
            }
        }
    }

    return nullptr;
}

CreatureTemplate const* BotDataMgr::GetBotExtraCreatureTemplate(uint32 entry)
{
    CreatureTemplateContainer::const_iterator cit = _botsWanderCreatureTemplates.find(entry);
    return cit == _botsWanderCreatureTemplates.cend() ? nullptr : &cit->second;
}

EquipmentInfo const* BotDataMgr::GetBotEquipmentInfo(uint32 entry)
{
    decltype(_botsWanderCreatureEquipmentTemplates)::const_iterator cit = _botsWanderCreatureEquipmentTemplates.find(entry);
    if (cit == _botsWanderCreatureEquipmentTemplates.cend())
    {
        static int8 eqId = 1;
        return sObjectMgr->GetEquipmentInfo(entry, eqId);
    }
    else
        return cit->second;
}

void BotDataMgr::AddNpcBotData(uint32 entry, uint32 roles, uint8 spec, uint32 faction)
{
    //botData must be allocated explicitly
    NpcBotDataMap::iterator itr = _botsData.find(entry);
    if (itr == _botsData.end())
    {
        NpcBotData* botData = new NpcBotData(roles, faction, spec);
        _botsData[entry] = botData;

        CharacterDatabasePreparedStatement* bstmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_NPCBOT);
        //"INSERT INTO characters_npcbot (entry, roles, spec, faction) VALUES (?, ?, ?, ?)", CONNECTION_ASYNC);
        bstmt->setUInt32(0, entry);
        bstmt->setUInt32(1, roles);
        bstmt->setUInt8(2, spec);
        bstmt->setUInt32(3, faction);
        CharacterDatabase.Execute(bstmt);

        return;
    }

    TC_LOG_ERROR("sql.sql", "BotMgr::AddNpcBotData(): trying to add new data but entry already exists! entry = %u", entry);
}
NpcBotData const* BotDataMgr::SelectNpcBotData(uint32 entry)
{
    NpcBotDataMap::const_iterator itr = _botsData.find(entry);
    return itr != _botsData.cend() ? itr->second : nullptr;
}
void BotDataMgr::UpdateNpcBotData(uint32 entry, NpcBotDataUpdateType updateType, void* data)
{
    NpcBotDataMap::iterator itr = _botsData.find(entry);
    if (itr == _botsData.end())
        return;

    CharacterDatabasePreparedStatement* bstmt;
    switch (updateType)
    {
        case NPCBOT_UPDATE_OWNER:
            if (itr->second->owner == *(uint32*)(data))
                break;
            itr->second->owner = *(uint32*)(data);
            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_NPCBOT_OWNER);
            //"UPDATE characters_npcbot SET owner = ? WHERE entry = ?", CONNECTION_ASYNC
            bstmt->setUInt32(0, itr->second->owner);
            bstmt->setUInt32(1, entry);
            CharacterDatabase.Execute(bstmt);
            //break; //no break: erase transmogs
        [[fallthrough]];
        case NPCBOT_UPDATE_TRANSMOG_ERASE:
            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_NPCBOT_TRANSMOG);
            //"DELETE FROM characters_npcbot_transmog WHERE entry = ?", CONNECTION_ASYNC
            bstmt->setUInt32(0, entry);
            CharacterDatabase.Execute(bstmt);
            break;
        case NPCBOT_UPDATE_ROLES:
            itr->second->roles = *(uint32*)(data);
            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_NPCBOT_ROLES);
            //"UPDATE character_npcbot SET roles = ? WHERE entry = ?", CONNECTION_ASYNC
            bstmt->setUInt32(0, itr->second->roles);
            bstmt->setUInt32(1, entry);
            CharacterDatabase.Execute(bstmt);
            break;
        case NPCBOT_UPDATE_SPEC:
            itr->second->spec = *(uint8*)(data);
            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_NPCBOT_SPEC);
            //"UPDATE characters_npcbot SET spec = ? WHERE entry = ?", CONNECTION_ASYNCH
            bstmt->setUInt8(0, itr->second->spec);
            bstmt->setUInt32(1, entry);
            CharacterDatabase.Execute(bstmt);
            break;
        case NPCBOT_UPDATE_FACTION:
            itr->second->faction = *(uint32*)(data);
            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_NPCBOT_FACTION);
            //"UPDATE characters_npcbot SET faction = ? WHERE entry = ?", CONNECTION_ASYNCH
            bstmt->setUInt32(0, itr->second->faction);
            bstmt->setUInt32(1, entry);
            CharacterDatabase.Execute(bstmt);
            break;
        case NPCBOT_UPDATE_DISABLED_SPELLS:
        {
            NpcBotData::DisabledSpellsContainer const* spells = (NpcBotData::DisabledSpellsContainer const*)(data);
            std::ostringstream ss;
            for (NpcBotData::DisabledSpellsContainer::const_iterator citr = spells->begin(); citr != spells->end(); ++citr)
                ss << (*citr) << ' ';

            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_NPCBOT_DISABLED_SPELLS);
            //"UPDATE characters_npcbot SET spells_disabled = ? WHERE entry = ?", CONNECTION_ASYNCH
            bstmt->setString(0, ss.str());
            bstmt->setUInt32(1, entry);
            CharacterDatabase.Execute(bstmt);
            break;
        }
        case NPCBOT_UPDATE_EQUIPS:
        {
            Item** items = (Item**)(data);

            EquipmentInfo const* einfo = BotDataMgr::GetBotEquipmentInfo(entry);

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_NPCBOT_EQUIP);
            //"UPDATE character_npcbot SET equipMhEx = ?, equipOhEx = ?, equipRhEx = ?, equipHead = ?, equipShoulders = ?, equipChest = ?, equipWaist = ?, equipLegs = ?,
            //equipFeet = ?, equipWrist = ?, equipHands = ?, equipBack = ?, equipBody = ?, equipFinger1 = ?, equipFinger2 = ?, equipTrinket1 = ?, equipTrinket2 = ?, equipNeck = ? WHERE entry = ?", CONNECTION_ASYNC
            CharacterDatabasePreparedStatement* stmt;
            uint8 k;
            for (k = BOT_SLOT_MAINHAND; k != BOT_INVENTORY_SIZE; ++k)
            {
                itr->second->equips[k] = items[k] ? items[k]->GetGUID().GetCounter() : 0;
                if (Item const* botitem = items[k])
                {
                    bool standard = false;
                    for (uint8 i = 0; i != MAX_EQUIPMENT_ITEMS; ++i)
                    {
                        if (einfo->ItemEntry[i] == botitem->GetEntry())
                        {
                            itr->second->equips[k] = 0;
                            bstmt->setUInt32(k, 0);
                            standard = true;
                            break;
                        }
                    }
                    if (standard)
                        continue;

                    uint8 index = 0;
                    stmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_ITEM_INSTANCE);
                    //REPLACE INTO item_instance (itemEntry, owner_guid, creatorGuid, giftCreatorGuid, count, duration, charges, flags, enchantments, randomPropertyId, durability, playedTime, text, guid)
                    //VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", CONNECTION_ASYNC : 0-13
                    stmt->setUInt32(  index, botitem->GetEntry());
                    stmt->setUInt32(++index, botitem->GetOwnerGUID().GetCounter());
                    stmt->setUInt32(++index, botitem->GetGuidValue(ITEM_FIELD_CREATOR).GetCounter());
                    stmt->setUInt32(++index, botitem->GetGuidValue(ITEM_FIELD_GIFTCREATOR).GetCounter());
                    stmt->setUInt32(++index, botitem->GetCount());
                    stmt->setUInt32(++index, botitem->GetUInt32Value(ITEM_FIELD_DURATION));

                    std::ostringstream ssSpells;
                    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
                        ssSpells << botitem->GetSpellCharges(i) << ' ';
                    stmt->setString(++index, ssSpells.str());

                    stmt->setUInt32(++index, botitem->GetUInt32Value(ITEM_FIELD_FLAGS));

                    std::ostringstream ssEnchants;
                    for (uint8 i = 0; i < MAX_ENCHANTMENT_SLOT; ++i)
                    {
                        ssEnchants << botitem->GetEnchantmentId(EnchantmentSlot(i)) << ' ';
                        ssEnchants << botitem->GetEnchantmentDuration(EnchantmentSlot(i)) << ' ';
                        ssEnchants << botitem->GetEnchantmentCharges(EnchantmentSlot(i)) << ' ';
                    }
                    stmt->setString(++index, ssEnchants.str());

                    stmt->setInt16 (++index, botitem->GetItemRandomPropertyId());
                    stmt->setUInt16(++index, botitem->GetUInt32Value(ITEM_FIELD_DURABILITY));
                    stmt->setUInt32(++index, botitem->GetUInt32Value(ITEM_FIELD_CREATE_PLAYED_TIME));
                    stmt->setString(++index, botitem->GetText());
                    stmt->setUInt32(++index, botitem->GetGUID().GetCounter());

                    trans->Append(stmt);

                    Item::DeleteFromInventoryDB(trans, botitem->GetGUID().GetCounter()); //prevent duplicates

                    bstmt->setUInt32(k, botitem->GetGUID().GetCounter());
                }
                else
                    bstmt->setUInt32(k, uint32(0));
            }

            bstmt->setUInt32(k, entry);
            trans->Append(bstmt);
            CharacterDatabase.CommitTransaction(trans);
            break;
        }
        case NPCBOT_UPDATE_ERASE:
        {
            NpcBotDataMap::iterator bitr = _botsData.find(entry);
            ASSERT(bitr != _botsData.end());
            delete bitr->second;
            _botsData.erase(bitr);
            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_NPCBOT);
            //"DELETE FROM characters_npcbot WHERE entry = ?", CONNECTION_ASYNC
            bstmt->setUInt32(0, entry);
            CharacterDatabase.Execute(bstmt);
            break;
        }
        default:
            TC_LOG_ERROR("sql.sql", "BotDataMgr:UpdateNpcBotData: unhandled updateType %u", uint32(updateType));
            break;
    }
}
void BotDataMgr::UpdateNpcBotDataAll(uint32 playerGuid, NpcBotDataUpdateType updateType, void* data)
{
    CharacterDatabasePreparedStatement* bstmt;
    switch (updateType)
    {
        case NPCBOT_UPDATE_OWNER:
            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_NPCBOT_OWNER_ALL);
            //"UPDATE characters_npcbot SET owner = ? WHERE owner = ?", CONNECTION_ASYNC
            bstmt->setUInt32(0, *(uint32*)(data));
            bstmt->setUInt32(1, playerGuid);
            CharacterDatabase.Execute(bstmt);
            //break; //no break: erase transmogs
        [[fallthrough]];
        case NPCBOT_UPDATE_TRANSMOG_ERASE:
            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_NPCBOT_TRANSMOG_ALL);
            //"DELETE FROM characters_npcbot_transmog WHERE entry IN (SELECT entry FROM characters_npcbot WHERE owner = ?)", CONNECTION_ASYNC
            bstmt->setUInt32(0, playerGuid);
            CharacterDatabase.Execute(bstmt);
            break;
        //case NPCBOT_UPDATE_ROLES:
        //case NPCBOT_UPDATE_FACTION:
        //case NPCBOT_UPDATE_EQUIPS:
        default:
            TC_LOG_ERROR("sql.sql", "BotDataMgr:UpdateNpcBotDataAll: unhandled updateType %u", uint32(updateType));
            break;
    }
}

void BotDataMgr::SaveNpcBotStats(NpcBotStats const* stats)
{
    CharacterDatabasePreparedStatement* bstmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_NPCBOT_STATS);
    //"REPLACE INTO characters_npcbot_stats
    //(entry, maxhealth, maxpower, strength, agility, stamina, intellect, spirit, armor, defense,
    //resHoly, resFire, resNature, resFrost, resShadow, resArcane, blockPct, dodgePct, parryPct, critPct,
    //attackPower, spellPower, spellPen, hastePct, hitBonusPct, expertise, armorPenPct) VALUES
    //(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", CONNECTION_ASYNC

    uint32 index = 0;
    bstmt->setUInt32(  index, stats->entry);
    bstmt->setUInt32(++index, stats->maxhealth);
    bstmt->setUInt32(++index, stats->maxpower);
    bstmt->setUInt32(++index, stats->strength);
    bstmt->setUInt32(++index, stats->agility);
    bstmt->setUInt32(++index, stats->stamina);
    bstmt->setUInt32(++index, stats->intellect);
    bstmt->setUInt32(++index, stats->spirit);
    bstmt->setUInt32(++index, stats->armor);
    bstmt->setUInt32(++index, stats->defense);
    bstmt->setUInt32(++index, stats->resHoly);
    bstmt->setUInt32(++index, stats->resFire);
    bstmt->setUInt32(++index, stats->resNature);
    bstmt->setUInt32(++index, stats->resFrost);
    bstmt->setUInt32(++index, stats->resShadow);
    bstmt->setUInt32(++index, stats->resArcane);
    bstmt->setFloat (++index, stats->blockPct);
    bstmt->setFloat (++index, stats->dodgePct);
    bstmt->setFloat (++index, stats->parryPct);
    bstmt->setFloat (++index, stats->critPct);
    bstmt->setUInt32(++index, stats->attackPower);
    bstmt->setUInt32(++index, stats->spellPower);
    bstmt->setUInt32(++index, stats->spellPen);
    bstmt->setFloat (++index, stats->hastePct);
    bstmt->setFloat (++index, stats->hitBonusPct);
    bstmt->setUInt32(++index, stats->expertise);
    bstmt->setFloat (++index, stats->armorPenPct);

    CharacterDatabase.Execute(bstmt);
}

NpcBotAppearanceData const* BotDataMgr::SelectNpcBotAppearance(uint32 entry)
{
    NpcBotAppearanceDataMap::const_iterator itr = _botsAppearanceData.find(entry);
    return itr != _botsAppearanceData.cend() ? itr->second : nullptr;
}

NpcBotExtras const* BotDataMgr::SelectNpcBotExtras(uint32 entry)
{
    NpcBotExtrasMap::const_iterator itr = _botsExtras.find(entry);
    return itr != _botsExtras.cend() ? itr->second : nullptr;
}

NpcBotTransmogData const* BotDataMgr::SelectNpcBotTransmogs(uint32 entry)
{
    NpcBotTransmogDataMap::const_iterator itr = _botsTransmogData.find(entry);
    return itr != _botsTransmogData.cend() ? itr->second : nullptr;
}
void BotDataMgr::UpdateNpcBotTransmogData(uint32 entry, uint8 slot, uint32 item_id, uint32 fake_id, bool update_db)
{
    ASSERT(slot < BOT_TRANSMOG_INVENTORY_SIZE);

    NpcBotTransmogDataMap::const_iterator itr = _botsTransmogData.find(entry);
    if (itr == _botsTransmogData.cend())
        _botsTransmogData[entry] = new NpcBotTransmogData();

    _botsTransmogData[entry]->transmogs[slot] = { item_id, fake_id };

    if (update_db)
    {
        CharacterDatabasePreparedStatement* bstmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_NPCBOT_TRANSMOG);
        //"REPLACE INTO characters_npcbot_transmog (entry, slot, item_id, fake_id) VALUES (?, ?, ?, ?)", CONNECTION_ASYNC
        bstmt->setUInt32(0, entry);
        bstmt->setUInt8(1, slot);
        bstmt->setUInt32(2, item_id);
        bstmt->setUInt32(3, fake_id);
        CharacterDatabase.Execute(bstmt);
    }
}

void BotDataMgr::ResetNpcBotTransmogData(uint32 entry, bool update_db)
{
    NpcBotTransmogDataMap::const_iterator itr = _botsTransmogData.find(entry);
    if (itr == _botsTransmogData.cend())
        return;

    if (update_db)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        for (uint8 i = 0; i != BOT_TRANSMOG_INVENTORY_SIZE; ++i)
        {
            if (_botsTransmogData[entry]->transmogs[i].first == 0 && _botsTransmogData[entry]->transmogs[i].second == 0)
                continue;

            CharacterDatabasePreparedStatement* bstmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_NPCBOT_TRANSMOG);
            //"REPLACE INTO characters_npcbot_transmog (entry, slot, item_id, fake_id) VALUES (?, ?, ?, ?)", CONNECTION_ASYNC
            bstmt->setUInt32(0, entry);
            bstmt->setUInt8(1, i);
            bstmt->setUInt32(2, 0);
            bstmt->setUInt32(3, 0);
            trans->Append(bstmt);
        }

        if (trans->GetSize() > 0)
            CharacterDatabase.CommitTransaction(trans);
    }

    for (uint8 i = 0; i != BOT_TRANSMOG_INVENTORY_SIZE; ++i)
        _botsTransmogData[entry]->transmogs[i] = { 0, 0 };
}

void BotDataMgr::RegisterBot(Creature const* bot)
{
    if (_existingBots.find(bot) != _existingBots.end())
    {
        TC_LOG_ERROR("entities.unit", "BotDataMgr::RegisterBot: bot %u (%s) already registered!",
            bot->GetEntry(), bot->GetName().c_str());
        return;
    }

    std::unique_lock<std::shared_mutex> lock(*GetLock());

    _existingBots.insert(bot);
    //TC_LOG_ERROR("entities.unit", "BotDataMgr::RegisterBot: registered bot %u (%s)", bot->GetEntry(), bot->GetName().c_str());
}
void BotDataMgr::UnregisterBot(Creature const* bot)
{
    if (_existingBots.find(bot) == _existingBots.end())
    {
        TC_LOG_ERROR("entities.unit", "BotDataMgr::UnregisterBot: bot %u (%s) not found!",
            bot->GetEntry(), bot->GetName().c_str());
        return;
    }

    std::unique_lock<std::shared_mutex> lock(*GetLock());

    _existingBots.erase(bot);
    //TC_LOG_ERROR("entities.unit", "BotDataMgr::UnregisterBot: unregistered bot %u (%s)", bot->GetEntry(), bot->GetName().c_str());
}
Creature const* BotDataMgr::FindBot(uint32 entry)
{
    std::shared_lock<std::shared_mutex> lock(*GetLock());

    for (NpcBotRegistry::const_iterator ci = _existingBots.cbegin(); ci != _existingBots.cend(); ++ci)
    {
        if ((*ci)->GetEntry() == entry)
            return *ci;
    }
    return nullptr;
}
Creature const* BotDataMgr::FindBot(std::string_view name, LocaleConstant loc)
{
    std::wstring wname;
    if (Utf8toWStr(name, wname))
    {
        wstrToLower(wname);
        std::shared_lock<std::shared_mutex> lock(*GetLock());
        for (NpcBotRegistry::const_iterator ci = _existingBots.cbegin(); ci != _existingBots.cend(); ++ci)
        {
            std::string basename = (*ci)->GetName();
            if (CreatureLocale const* creatureInfo = sObjectMgr->GetCreatureLocale((*ci)->GetEntry()))
            {
                if (creatureInfo->Name.size() > loc && !creatureInfo->Name[loc].empty())
                    basename = creatureInfo->Name[loc];
            }

            std::wstring wbname;
            if (!Utf8toWStr(basename, wbname))
                continue;

            wstrToLower(wbname);
            if (wbname == wname)
                return *ci;
        }
    }

    return nullptr;
}

NpcBotRegistry const& BotDataMgr::GetExistingNPCBots()
{
    return _existingBots;
}

void BotDataMgr::GetNPCBotGuidsByOwner(std::vector<ObjectGuid> &guids_vec, ObjectGuid owner_guid)
{
    ASSERT(AllBotsLoaded());

    std::shared_lock<std::shared_mutex> lock(*GetLock());

    for (NpcBotRegistry::const_iterator ci = _existingBots.cbegin(); ci != _existingBots.cend(); ++ci)
    {
        if (_botsData[(*ci)->GetEntry()]->owner == owner_guid.GetCounter())
            guids_vec.push_back((*ci)->GetGUID());
    }
}

ObjectGuid BotDataMgr::GetNPCBotGuid(uint32 entry)
{
    ASSERT(AllBotsLoaded());

    std::shared_lock<std::shared_mutex> lock(*GetLock());

    for (NpcBotRegistry::const_iterator ci = _existingBots.cbegin(); ci != _existingBots.cend(); ++ci)
    {
        if ((*ci)->GetEntry() == entry)
            return (*ci)->GetGUID();
    }

    return ObjectGuid::Empty;
}

std::vector<uint32> BotDataMgr::GetExistingNPCBotIds()
{
    ASSERT(AllBotsLoaded());

    std::vector<uint32> existing_ids;
    existing_ids.reserve(_botsData.size());
    for (decltype(_botsData)::value_type const& bot_data_pair : _botsData)
        existing_ids.push_back(bot_data_pair.first);

    return existing_ids;
}

uint8 BotDataMgr::GetOwnedBotsCount(ObjectGuid owner_guid, uint32 class_mask)
{
    uint8 count = 0;
    for (decltype(_botsData)::value_type const& bdata : _botsData)
        if (bdata.second->owner == owner_guid.GetCounter() && (!class_mask || !!(class_mask & (1u << (_botsExtras[bdata.first]->bclass - 1)))))
            ++count;

    return count;
}

uint8 BotDataMgr::GetLevelBonusForBotRank(uint32 rank)
{
    switch (rank)
    {
        case CREATURE_ELITE_RARE:
            return 1;
        case CREATURE_ELITE_ELITE:
            return 2;
        case CREATURE_ELITE_RAREELITE:
            return 3;
        default:
            return 0;
    }
}

uint8 BotDataMgr::GetMaxLevelForMapId(uint32 mapId)
{
    switch (mapId)
    {
        case 0:
        case 1:
            return 60;
        case 530:
            return 70;
        case 571:
            return 80;
        default:
            return 80;
    }
}

uint8 BotDataMgr::GetMinLevelForBotClass(uint8 m_class)
{
    switch (m_class)
    {
        case BOT_CLASS_DEATH_KNIGHT:
            return 55;
        case BOT_CLASS_ARCHMAGE:
        case BOT_CLASS_SPELLBREAKER:
        case BOT_CLASS_NECROMANCER:
            return 20;
        case BOT_CLASS_DARK_RANGER:
            return 40;
        case BOT_CLASS_SPHYNX:
        case BOT_CLASS_DREADLORD:
            return 60;
        default:
            return 1;
    }
}

int32 BotDataMgr::GetBotBaseReputation(Creature const* bot, FactionEntry const* factionEntry)
{
    if (!factionEntry)
        return 0;

    if (bot->IsNPCBotPet())
        bot = bot->GetBotPetAI()->GetPetsOwner();

    uint32 raceMask = bot->GetFaction() == 14 ? 0 : bot->GetRaceMask();
    uint32 classMask = bot->GetClassMask();

    int32 minRep = 42999;
    for (uint8 i = 0; i < 4; ++i)
    {
        if (raceMask == 0)
            minRep = std::min<int32>(minRep, factionEntry->ReputationBase[i]);
        if ((factionEntry->ReputationRaceMask[i] & raceMask || (factionEntry->ReputationRaceMask[i] == 0 && factionEntry->ReputationClassMask[i] != 0)) &&
            (factionEntry->ReputationClassMask[i] & classMask || factionEntry->ReputationClassMask[i] == 0))
        {
            return factionEntry->ReputationBase[i];
        }
    }

    return std::min<int32>(minRep, 0);
}

TeamId BotDataMgr::GetTeamForFaction(uint32 factionTemplateId)
{
    if (FactionTemplateEntry const* fte = sFactionTemplateStore.LookupEntry(factionTemplateId))
    {
        if (fte->FactionGroup & FACTION_MASK_ALLIANCE)
            return TEAM_ALLIANCE;
        else if (fte->FactionGroup & FACTION_MASK_HORDE)
            return TEAM_HORDE;
    }

    return TEAM_NEUTRAL;
}

bool BotDataMgr::IsWanderNodeAvailableForBotFaction(WanderNode const* wp, uint32 factionTemplateId)
{
    switch (GetTeamForFaction(factionTemplateId))
    {
        case TEAM_ALLIANCE:
            return !wp->HasFlag(BotWPFlags::BOTWP_FLAG_HORDE_ONLY);
        case TEAM_HORDE:
            return !wp->HasFlag(BotWPFlags::BOTWP_FLAG_ALLIANCE_ONLY);
        case TEAM_NEUTRAL:
            return !wp->HasFlag(BotWPFlags::BOTWP_FLAG_ALLIANCE_OR_HORDE_ONLY);
        default:
            return true;
    }
}

WanderNode const* BotDataMgr::GetNextWanderNode(WanderNode const* curNode, WanderNode const* lastNode, Position const* curPos, uint32 faction, uint32 lvl, bool random)
{
    static auto node_viable = [](WanderNode const* wp, uint8 lvl) {
        return lvl + 2 >= wp->GetLevels().first && lvl <= wp->GetLevels().second;
    };

    //Node got deleted (or forced)! Select close point and go from there
    std::list<WanderNode const*> links;
    if (curNode->GetLinks().empty() || random)
    {
        WanderNode::DoForAllMapWPs(curNode->GetMapId(), [&links, lvl = lvl, fac = faction, pos = curPos](WanderNode const* wp) {
            if (pos->GetExactDist2d(wp) < MAX_WANDER_NODE_DISTANCE &&
                IsWanderNodeAvailableForBotFaction(wp, fac) && node_viable(wp, lvl))
                links.push_back(wp);
        });
        if (!links.empty())
            return links.size() == 1u ? links.front() : Trinity::Containers::SelectRandomContainerElement(links);

        //Select closest
        WanderNode const* node_new = nullptr;
        float mindist = 50000.0f; // Anywhere
        WanderNode::DoForAllMapWPs(curNode->GetMapId(), [&node_new, &mindist, lvl = lvl, fac = faction, pos = curPos](WanderNode const* wp) {
            float dist = pos->GetExactDist2d(wp);
            if (dist < mindist &&
                IsWanderNodeAvailableForBotFaction(wp, fac) && node_viable(wp, lvl))
            {
                mindist = dist;
                node_new = wp;
            }
        });
        return node_new;
    }

    for (WanderNode const* wp : curNode->GetLinks())
    {
        if (IsWanderNodeAvailableForBotFaction(wp, faction) && node_viable(wp, lvl))
            links.push_back(wp);
    }
    if (links.size() > 1 && lastNode)
        links.remove(lastNode);

    //Overleveled or died: no viable nodes in reach, find one for teleport
    if (links.empty())
    {
        //todo: use all wps
        WanderNode::DoForAllMapWPs(curNode->GetMapId(), [&links, lvl = lvl, fac = faction](WanderNode const* wp) {
            if (IsWanderNodeAvailableForBotFaction(wp, fac) && wp->HasFlag(BotWPFlags::BOTWP_FLAG_SPAWN) && node_viable(wp, lvl))
                links.push_back(wp);
        });
    }

    ASSERT(!links.empty());
    return links.size() == 1u ? links.front() : Trinity::Containers::SelectRandomContainerElement(links);
}

WanderNode const* BotDataMgr::GetClosestWanderNode(WorldLocation const* loc)
{
    float mindist = 50000.0f;
    WanderNode const* closestNode = nullptr;
    WanderNode::DoForAllMapWPs(loc->GetMapId(), [&mindist, &closestNode, loc = loc](WanderNode const* wp) {
        float dist = wp->GetExactDist2d(loc);
        if (dist < mindist)
        {
            mindist = dist;
            closestNode = wp;
        }
    });

    return closestNode;
}

#ifdef _MSC_VER
# pragma warning(pop)
#endif
