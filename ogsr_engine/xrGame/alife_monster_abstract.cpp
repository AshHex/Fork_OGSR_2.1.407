////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_monster_abstract.cpp
//	Created 	: 27.10.2005
//  Modified 	: 27.10.2005
//	Author		: Dmitriy Iassenev
//	Description : ALife monster abstract class
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "alife_runtime_switch.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_combat_manager.h"
#include "alife_group_registry.h"
#include "relation_registry.h"
#include "alife_time_manager.h"
#include "alife_graph_registry.h"
#include "game_graph.h"
#include "alife_object_registry.h"
#include "ef_storage.h"
#include "ef_pattern.h"
#include "alife_monster_brain.h"
#include "alife_monster_movement_manager.h"
#include "alife_monster_detail_path_manager.h"

void CSE_ALifeMonsterAbstract::add_online(const bool& update_registries)
{
    inherited1::add_online(update_registries);
    brain().on_switch_online();
}

void CSE_ALifeMonsterAbstract::add_offline(const xr_vector<ALife::_OBJECT_ID>& saved_children, const bool& update_registries)
{
    inherited1::add_offline(saved_children, update_registries);
    brain().on_switch_offline();
}

void CSE_ALifeMonsterAbstract::on_register()
{
    inherited1::on_register();
    brain().on_register();
}

void CSE_ALifeMonsterAbstract::on_unregister()
{
    inherited1::on_unregister();

    if (!ai().alife().is_unloading())
        RELATION_REGISTRY().ClearRelations(ID);

    brain().on_unregister();

    if (m_group_id != 0xffff)
        ai().alife().groups().object(m_group_id).unregister_member(ID);
}

void CSE_ALifeMonsterAbstract::update()
{
    if (!bfActive())
        return;

    const GameGraph::_GRAPH_ID previous_graph_id = m_tGraphID;

    brain().update();

    if (ALIFE_ON && !m_bOnline && bfActive() && m_story_id == INVALID_STORY_ID &&
        m_tGraphID != previous_graph_id)
    {
        alife().check_for_interaction(this);
    }
}

CSE_ALifeItemWeapon* CSE_ALifeMonsterAbstract::tpfGetBestWeapon(ALife::EHitType& tHitType, float& fHitPower)
{
    m_tpCurrentBestWeapon = nullptr;
    fHitPower = m_fHitPower;
    tHitType = m_tHitType;
    return m_tpCurrentBestWeapon;
}

ALife::EMeetActionType CSE_ALifeMonsterAbstract::tfGetActionType(CSE_ALifeSchedulable* tpALifeSchedulable, int iGroupIndex, bool bMutualDetection)
{
    if (!ALIFE_ON)
        return ALife::eMeetActionTypeIgnore;

    if (!tpALifeSchedulable || m_story_id != INVALID_STORY_ID)
        return ALife::eMeetActionTypeIgnore;

    if (CSE_ALifeHumanAbstract* human = smart_cast<CSE_ALifeHumanAbstract*>(tpALifeSchedulable))
    {
        if (human->m_story_id != INVALID_STORY_ID)
            return ALife::eMeetActionTypeIgnore;
    }
    else if (CSE_ALifeMonsterAbstract* monster = smart_cast<CSE_ALifeMonsterAbstract*>(tpALifeSchedulable))
    {
        if (monster->m_story_id != INVALID_STORY_ID)
            return ALife::eMeetActionTypeIgnore;
    }

    switch (ai().alife().combat_type())
    {
    case ALife::eCombatTypeMonsterMonster:
    {
        CSE_ALifeMonsterAbstract* other = smart_cast<CSE_ALifeMonsterAbstract*>(tpALifeSchedulable);
        R_ASSERT2(other, "Inconsistent meet action type: expected a monster");

        const ALife::ERelationType relation = ai().alife().relation_type(this, other);

        if (relation != ALife::eRelationTypeEnemy)
            return ALife::eMeetActionTypeIgnore;

        if (bMutualDetection || alife().choose_combat_action(iGroupIndex) == ALife::eCombatActionAttack)
            return ALife::eMeetActionTypeAttack;

        return ALife::eMeetActionTypeIgnore;
    }

    case ALife::eCombatTypeSmartTerrain:
    {
        CSE_ALifeSmartZone* smart_zone = smart_cast<CSE_ALifeSmartZone*>(tpALifeSchedulable);
        VERIFY(smart_zone);

        return smart_zone->tfGetActionType(this, iGroupIndex ? 0 : 1, bMutualDetection);
    }

    case ALife::eCombatTypeMonsterAnomaly:
    case ALife::eCombatTypeAnomalyMonster:
    default:
        return ALife::eMeetActionTypeAttack;
    }
}

bool CSE_ALifeMonsterAbstract::bfActive()
{
    CSE_ALifeGroupAbstract* group = smart_cast<CSE_ALifeGroupAbstract*>(this);
    return interactive() && ((group && group->m_wCount > 0) || (!group && fHealth > EPS_L));
}

CSE_ALifeDynamicObject* CSE_ALifeMonsterAbstract::tpfGetBestDetector()
{
    CSE_ALifeGroupAbstract* group = smart_cast<CSE_ALifeGroupAbstract*>(this);
    if (!group)
        return this;

    if (!group->m_wCount || group->m_tpMembers.empty())
        return nullptr;

    CSE_ALifeDynamicObject* member = ai().alife().objects().object(group->m_tpMembers.front(), true);
    return member;
}

Fvector CSE_ALifeMonsterAbstract::draw_level_position() const
{
    return brain().movement().detail().draw_level_position();
}

bool CSE_ALifeMonsterAbstract::redundant() const
{
    if (g_Alive())
        return false;

    if (m_bOnline)
        return false;

    if (m_story_id != INVALID_STORY_ID)
        return false;

    if (!m_game_death_time)
        return false;

    const ALife::_TIME_ID current_time = alife().time_manager().game_time();
    VERIFY(m_game_death_time <= current_time);

    if ((m_game_death_time + m_stay_after_death_time_interval) > current_time)
        return false;

    return true;
}
