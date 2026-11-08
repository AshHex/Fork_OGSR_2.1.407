////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_combat_manager.cpp
//	Created 	: 12.08.2003
//  Modified 	: 14.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife combat manager
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "alife_runtime_switch.h"
#include "alife_combat_manager.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "alife_graph_registry.h"
#include "alife_schedule_registry.h"
#include "relation_registry.h"
#include "monster_community.h"
#include "ef_storage.h"
#include "ef_pattern.h"
#include "alife_news_queue.h"
#include "alife_object_registry.h"
#include "alife_simulator.h"
#include "alife_spawn_registry.h"
#include "alife_time_manager.h"
#include "relation_registry.h"

using namespace ALife;

namespace
{
IC bool has_story_id(CSE_ALifeSchedulable* object)
{
    if (!object)
        return false;

    if (CSE_ALifeHumanAbstract* human = smart_cast<CSE_ALifeHumanAbstract*>(object))
        return human->m_story_id != INVALID_STORY_ID;

    if (CSE_ALifeMonsterAbstract* monster = smart_cast<CSE_ALifeMonsterAbstract*>(object))
        return monster->m_story_id != INVALID_STORY_ID;

    return false;
}

IC float clamp_probability(float value)
{
    if (value < 0.f)
        return 0.f;
    if (value > 100.f)
        return 100.f;
    return value;
}

IC float clamp_unit(float value)
{
    if (value < 0.f)
        return 0.f;
    if (value > 1.f)
        return 1.f;
    return value;
}
}

CALifeCombatManager::CALifeCombatManager(xrServer* server, LPCSTR section)
    : CALifeSimulatorBase(server, section),
      m_dwMaxCombatIterationCount(READ_IF_EXISTS(pSettings, r_u32, section, "max_combat_iteration_count", 5u)),
      m_combat_type(eCombatTypeMonsterMonster)
{
    m_tpaCombatObjects[0] = nullptr;
    m_tpaCombatObjects[1] = nullptr;
    m_tpGraphPointObjects.clear();
    m_tpTempItemBuffer.clear();

    if (!m_dwMaxCombatIterationCount)
        m_dwMaxCombatIterationCount = 1;

    for (int i = 0; i < 2; ++i)
    {
        m_tpaCombatGroups[i].clear();
        m_tpaCombatGroups[i].reserve(255);
    }
}

CALifeCombatManager::~CALifeCombatManager() = default;

void CALifeCombatManager::vfFillCombatGroup(CSE_ALifeSchedulable* tpALifeSchedulable, int iGroupIndex)
{
    if (!tpALifeSchedulable || (iGroupIndex < 0) || (iGroupIndex > 1))
        return;

    SCHEDULE_P_VECTOR& group = m_tpaCombatGroups[iGroupIndex];
    group.clear();

    if (has_story_id(tpALifeSchedulable))
    {
        m_tpaCombatObjects[iGroupIndex] = tpALifeSchedulable;
        return;
    }

    CSE_ALifeGroupAbstract* group_abstract = smart_cast<CSE_ALifeGroupAbstract*>(tpALifeSchedulable);
    if (group_abstract)
    {
        for (const auto member_id : group_abstract->m_tpMembers)
        {
            CSE_ALifeSchedulable* member = smart_cast<CSE_ALifeSchedulable*>(objects().object(member_id, true));
            if (!member || !member->bfActive() || has_story_id(member))
                continue;

            group.push_back(member);

            EHitType hit_type = eHitTypeMax;
            float hit_power = 0.f;
            member->tpfGetBestWeapon(hit_type, hit_power);
        }
    }
    else if (tpALifeSchedulable->bfActive())
    {
        group.push_back(tpALifeSchedulable);

        EHitType hit_type = eHitTypeMax;
        float hit_power = 0.f;
        tpALifeSchedulable->tpfGetBestWeapon(hit_type, hit_power);
    }

    m_tpaCombatObjects[iGroupIndex] = tpALifeSchedulable;
}

ALife::ECombatAction CALifeCombatManager::choose_combat_action(int iCombatGroupIndex)
{
    if ((iCombatGroupIndex < 0) || (iCombatGroupIndex > 1) || m_tpaCombatGroups[iCombatGroupIndex].empty())
        return eCombatActionRetreat;

    ai().ef_storage().alife_evaluation(true);

    if (eCombatTypeMonsterMonster != combat_type())
    {
        if (((eCombatTypeAnomalyMonster == combat_type()) && !iCombatGroupIndex) ||
            ((eCombatTypeMonsterAnomaly == combat_type()) && iCombatGroupIndex))
            return eCombatActionAttack;

        return eCombatActionRetreat;
    }

    SCHEDULE_P_VECTOR& members = m_tpaCombatGroups[iCombatGroupIndex];
    SCHEDULE_P_VECTOR& enemies = m_tpaCombatGroups[iCombatGroupIndex ^ 1];

    if (members.empty() || enemies.empty())
        return eCombatActionAttack;

    const CSE_ALifeMonsterAbstract* first_member = smart_cast<CSE_ALifeMonsterAbstract*>(members.front());
    if (!first_member)
        return eCombatActionRetreat;

    const float min_probability = clamp_unit(first_member->m_fRetreatThreshold);

    int i = 0;
    int j = 0;
    const int I = static_cast<int>(members.size());
    const int J = static_cast<int>(enemies.size());

    while ((i < I) && (j < J))
    {
        ai().ef_storage().alife().member() = smart_cast<CSE_ALifeMonsterAbstract*>(members[i]);
        ai().ef_storage().alife().enemy() = smart_cast<CSE_ALifeMonsterAbstract*>(enemies[j]);

        if (!ai().ef_storage().alife().member() || !ai().ef_storage().alife().enemy())
            break;

        float probability = clamp_unit(ai().ef_storage().m_pfVictoryProbability->ffGetValue() / 100.f);
        float current_probability;

        if (probability > min_probability)
        {
            current_probability = probability;
            for (++j; (i < I) && (j < J); ++j)
            {
                ai().ef_storage().alife().enemy() = smart_cast<CSE_ALifeMonsterAbstract*>(enemies[j]);
                if (!ai().ef_storage().alife().enemy())
                    break;

                probability = clamp_unit(ai().ef_storage().m_pfVictoryProbability->ffGetValue() / 100.f);
                if (current_probability * probability < min_probability)
                {
                    ++i;
                    break;
                }
                current_probability *= probability;
            }
        }
        else
        {
            current_probability = 1.f - probability;
            for (++i; (i < I) && (j < J); ++i)
            {
                ai().ef_storage().alife().member() = smart_cast<CSE_ALifeMonsterAbstract*>(members[i]);
                if (!ai().ef_storage().alife().member())
                    break;

                probability = 1.f - clamp_unit(ai().ef_storage().m_pfVictoryProbability->ffGetValue() / 100.f);
                if (current_probability * probability < min_probability)
                {
                    ++j;
                    break;
                }
                current_probability *= probability;
            }
        }
    }

    return (j >= J) ? eCombatActionAttack : eCombatActionRetreat;
}

bool CALifeCombatManager::bfCheckObjectDetection(CSE_ALifeSchedulable* tpALifeSchedulable1,
    CSE_ALifeSchedulable* tpALifeSchedulable2)
{
    if (!tpALifeSchedulable1 || !tpALifeSchedulable2)
        return false;

    ai().ef_storage().alife_evaluation(true);

    switch (combat_type())
    {
    case eCombatTypeMonsterMonster:
        ai().ef_storage().alife().member_item() = smart_cast<CSE_ALifeObject*>(tpALifeSchedulable1);
        ai().ef_storage().alife().member() = tpALifeSchedulable1;
        ai().ef_storage().alife().enemy() = tpALifeSchedulable2;
        return Random.randF(100.f) < clamp_probability(ai().ef_storage().m_pfEnemyDetectProbability->ffGetValue());

    case eCombatTypeAnomalyMonster:
        ai().ef_storage().alife().enemy() = tpALifeSchedulable1;
        ai().ef_storage().alife().member() = tpALifeSchedulable1;
        return Random.randF(100.f) < clamp_probability(ai().ef_storage().m_pfAnomalyInteractProbability->ffGetValue());

    case eCombatTypeMonsterAnomaly:
        ai().ef_storage().alife().member_item() = tpALifeSchedulable1->tpfGetBestDetector();
        if (!ai().ef_storage().alife().member_item())
            return false;
        ai().ef_storage().alife().member() = tpALifeSchedulable1;
        ai().ef_storage().alife().enemy() = tpALifeSchedulable2;
        return Random.randF(100.f) < clamp_probability(ai().ef_storage().m_pfAnomalyDetectProbability->ffGetValue());

    case eCombatTypeSmartTerrain:
    {
        CSE_ALifeSmartZone* smart_zone = smart_cast<CSE_ALifeSmartZone*>(tpALifeSchedulable1);
        if (!smart_zone)
            return false;
        return Random.randF(100.f) < clamp_probability(100.f * smart_zone->detect_probability());
    }

    default:
        return false;
    }
}

bool CALifeCombatManager::bfCheckForInteraction(CSE_ALifeSchedulable* tpALifeSchedulable1,
    CSE_ALifeSchedulable* tpALifeSchedulable2, int& iCombatGroupIndex, bool& bMutualDetection)
{
    iCombatGroupIndex = -1;

    if (!ALIFE_ON)
        return false;
    bMutualDetection = false;

    if (!tpALifeSchedulable1 || !tpALifeSchedulable2 || !tpALifeSchedulable1->bfActive() || !tpALifeSchedulable2->bfActive())
        return false;

    if (has_story_id(tpALifeSchedulable1) || has_story_id(tpALifeSchedulable2))
        return false;

    CSE_ALifeMonsterAbstract* monster1 = smart_cast<CSE_ALifeMonsterAbstract*>(tpALifeSchedulable1);
    CSE_ALifeMonsterAbstract* monster2 = smart_cast<CSE_ALifeMonsterAbstract*>(tpALifeSchedulable2);

    if (!monster1)
    {
        if (!monster2)
            return false;

        if (!smart_cast<CSE_ALifeCustomZone*>(tpALifeSchedulable1))
            return false;

        m_combat_type = eCombatTypeAnomalyMonster;
    }
    else if (!monster2)
    {
        if (smart_cast<CSE_ALifeCustomZone*>(tpALifeSchedulable2))
            m_combat_type = eCombatTypeMonsterAnomaly;
        else if (smart_cast<CSE_ALifeSmartZone*>(tpALifeSchedulable2))
            m_combat_type = eCombatTypeSmartTerrain;
        else
            return false;
    }
    else
    {
        m_combat_type = eCombatTypeMonsterMonster;

        if (eRelationTypeFriend == relation_type(monster1, monster2))
        {
            if (smart_cast<CSE_ALifeHumanAbstract*>(monster1) && smart_cast<CSE_ALifeHumanAbstract*>(monster2))
            {
                iCombatGroupIndex = 0;
                return true;
            }
            return false;
        }
    }

    const ECombatType original_type = m_combat_type;

    if (bfCheckObjectDetection(tpALifeSchedulable1, tpALifeSchedulable2))
        iCombatGroupIndex = 0;

    if (eCombatTypeMonsterAnomaly == original_type)
        m_combat_type = eCombatTypeAnomalyMonster;
    else if (eCombatTypeAnomalyMonster == original_type)
        m_combat_type = eCombatTypeMonsterAnomaly;

    if (bfCheckObjectDetection(tpALifeSchedulable2, tpALifeSchedulable1))
    {
        if (iCombatGroupIndex == 0)
            bMutualDetection = true;
        else
            iCombatGroupIndex = 1;
    }

    m_combat_type = original_type;
    return iCombatGroupIndex >= 0;
}

bool CALifeCombatManager::bfCheckIfRetreated(int iCombatGroupIndex)
{
    if ((iCombatGroupIndex < 0) || (iCombatGroupIndex > 1) ||
        m_tpaCombatGroups[iCombatGroupIndex].empty() || m_tpaCombatGroups[iCombatGroupIndex ^ 1].empty())
        return false;

    CSE_ALifeSchedulable* member = m_tpaCombatGroups[iCombatGroupIndex].front();
    CSE_ALifeSchedulable* enemy = m_tpaCombatGroups[iCombatGroupIndex ^ 1].front();
    if (!member || !enemy)
        return false;

    ai().ef_storage().alife_evaluation(true);
    ai().ef_storage().alife().member() = member;
    ai().ef_storage().alife().enemy() = enemy;

    if (eCombatTypeMonsterMonster == combat_type())
    {
        ai().ef_storage().alife().member_item() = smart_cast<CSE_ALifeObject*>(member);
        return Random.randF(100.f) < clamp_probability(ai().ef_storage().m_pfEnemyRetreatProbability->ffGetValue());
    }

    ai().ef_storage().alife().member_item() = smart_cast<CSE_ALifeObject*>(member);
    return Random.randF(100.f) < clamp_probability(ai().ef_storage().m_pfAnomalyRetreatProbability->ffGetValue());
}

void CALifeCombatManager::vfPerformAttackAction(int iCombatGroupIndex)
{
    if (!ALIFE_ON)
        return;

    if ((iCombatGroupIndex < 0) || (iCombatGroupIndex > 1) ||
        m_tpaCombatGroups[iCombatGroupIndex].empty() || m_tpaCombatGroups[iCombatGroupIndex ^ 1].empty())
        return;

    ai().ef_storage().alife_evaluation(true);

    SCHEDULE_P_VECTOR& combat_group = m_tpaCombatGroups[iCombatGroupIndex];
    SCHEDULE_P_VECTOR& enemy_group = m_tpaCombatGroups[iCombatGroupIndex ^ 1];

    for (SCHEDULE_P_IT I = combat_group.begin(), E = combat_group.end(); I != E; ++I)
    {
        CSE_ALifeSchedulable* attacker = *I;
        if (!attacker || !attacker->bfActive() || has_story_id(attacker))
            continue;

        EHitType hit_type = eHitTypeMax;
        float hit_power = 0.f;
        CSE_ALifeItemWeapon* weapon = attacker->tpfGetBestWeapon(hit_type, hit_power);

        CSE_ALifeMonsterAbstract* attacker_monster = smart_cast<CSE_ALifeMonsterAbstract*>(attacker);
        if ((!weapon || hit_power <= EPS_L) && attacker_monster)
        {
            hit_power = attacker_monster->m_fHitPower;
            hit_type = attacker_monster->m_tHitType;
        }

        if (hit_power <= EPS_L)
            continue;

        if (hit_type >= eHitTypeMax)
            hit_type = eHitTypeWound;

        ai().ef_storage().alife().member_item() = smart_cast<CSE_ALifeObject*>(attacker);
        ai().ef_storage().alife().member() = attacker;

        int attack_times = iFloor(ai().ef_storage().m_pfWeaponAttackTimes->ffGetValue() + .5f);
        if (attack_times <= 0)
            continue;
        attack_times = _min(attack_times, 100);

        for (int i = 0; i < attack_times; ++i)
        {
            if (enemy_group.empty())
                return;

            if (Random.randF(100.f) < clamp_probability(ai().ef_storage().m_pfWeaponSuccessProbability->ffGetValue()))
            {
                const int enemy_index = Random.randI(static_cast<s32>(enemy_group.size()));
                CSE_ALifeMonsterAbstract* victim = smart_cast<CSE_ALifeMonsterAbstract*>(enemy_group[enemy_index]);
                if (!victim || has_story_id(victim))
                    continue;

                const float hit = Random.randF(hit_power * .5f, hit_power * 1.5f);
                const float immunity = _max(victim->m_fpImmunityFactors[hit_type], 0.f);
                const float damage = immunity * _max(hit, 0.f);

                victim->fHealth -= damage;

                if (victim->fHealth <= EPS_L)
                {
                    enemy_group.erase(enemy_group.begin() + enemy_index);
                    if (enemy_group.empty())
                        return;
                }
            }

            if (!attacker->bfPerformAttack())
                break;
        }
    }
}

void CALifeCombatManager::vfFinishCombat(ECombatResult tCombatResult)
{
    if (!ALIFE_ON)
        return;

    CSE_ALifeDynamicObject* first_dynamic = smart_cast<CSE_ALifeDynamicObject*>(m_tpaCombatObjects[0]);
    if (!first_dynamic)
    {
        m_temp_item_vector.clear();
        return;
    }

    const GameGraph::_GRAPH_ID graph_id = first_dynamic->m_tGraphID;
    m_temp_item_vector.clear();

    for (int i = 0; i < 2; ++i)
    {
        CSE_ALifeGroupAbstract* group = smart_cast<CSE_ALifeGroupAbstract*>(m_tpaCombatObjects[i]);
        if (group)
        {
            for (int I = 0; I < static_cast<int>(group->m_tpMembers.size()); )
            {
                const ALife::_OBJECT_ID member_id = group->m_tpMembers[I];
                CSE_ALifeMonsterAbstract* monster =
                    smart_cast<CSE_ALifeMonsterAbstract*>(objects().object(member_id, true));

                if (!monster)
                {
                    group->m_tpMembers.erase(group->m_tpMembers.begin() + I);
                    group->m_wCount = static_cast<u16>(group->m_tpMembers.size());
                    continue;
                }

                if (has_story_id(monster))
                {
                    ++I;
                    continue;
                }

                monster->vfUpdateWeaponAmmo();

                if (monster->fHealth <= EPS_L)
                {
                    append_item_vector(monster->children, m_temp_item_vector);
                    monster->m_bDirectControl = true;
                    group->m_tpMembers.erase(group->m_tpMembers.begin() + I);
                    group->m_wCount = static_cast<u16>(group->m_tpMembers.size());

                    CSE_ALifeSchedulable* killer = m_tpaCombatObjects[i ^ 1];
                    if (killer)
                    {
                        ALifeNewsQueue::push(
                            "kill",
                            killer->base()->name_replace(),
                            monster->base()->name_replace());
                    }

                    assign_death_position(monster, graph_id, killer);
                    monster->vfDetachAll();
                    register_object(monster);

                    CSE_ALifeInventoryItem* inventory_item = smart_cast<CSE_ALifeInventoryItem*>(monster);
                    if (inventory_item)
                        m_temp_item_vector.push_back(inventory_item);
                    continue;
                }

                ++I;
            }
        }
        else if (m_tpaCombatObjects[i])
        {
            m_tpaCombatObjects[i]->vfUpdateWeaponAmmo();

            CSE_ALifeMonsterAbstract* monster = smart_cast<CSE_ALifeMonsterAbstract*>(m_tpaCombatObjects[i]);
            if (monster && !has_story_id(monster) && monster->fHealth <= EPS_L)
                kill_entity(monster, graph_id, m_tpaCombatObjects[i ^ 1]);
        }
    }

    if (m_temp_item_vector.empty() || (eCombatTypeMonsterMonster != combat_type()))
    {
        m_temp_item_vector.clear();
        return;
    }

    int winner_index = -1;
    switch (tCombatResult)
    {
    case eCombatResultBothKilled:
    case eCombatResultRetreat12:
        break;

    case eCombatResultRetreat2:
    case eCombatResult1Kill2:
        winner_index = 0;
        break;

    case eCombatResultRetreat1:
    case eCombatResult2Kill1:
        winner_index = 1;
        break;

    default:
        break;
    }

    if ((winner_index >= 0) && (winner_index <= 1) && m_tpaCombatObjects[winner_index])
    {
        CSE_ALifeMonsterAbstract* winner =
            smart_cast<CSE_ALifeMonsterAbstract*>(m_tpaCombatObjects[winner_index]);
        if (winner && winner->g_Alive())
            winner->vfAttachItems();
    }

    m_temp_item_vector.clear();
}

namespace
{
ALife::ERelationType get_monster_relation(CSE_ALifeMonsterAbstract* object1, CSE_ALifeMonsterAbstract* object2)
{
    if (!object1 || !object2)
        return ALife::eRelationTypeNeutral;

    CSE_ALifeMonsterAbstract* relation_object1 = object1;
    CSE_ALifeMonsterAbstract* relation_object2 = object2;

    if (CSE_ALifeGroupAbstract* group = smart_cast<CSE_ALifeGroupAbstract*>(object1))
    {
        if (group->m_tpMembers.empty())
            return ALife::eRelationTypeNeutral;

        relation_object1 = smart_cast<CSE_ALifeMonsterAbstract*>(
            ai().alife().objects().object(group->m_tpMembers.front(), true));
        if (!relation_object1)
            return ALife::eRelationTypeNeutral;
    }

    if (CSE_ALifeGroupAbstract* group = smart_cast<CSE_ALifeGroupAbstract*>(object2))
    {
        if (group->m_tpMembers.empty())
            return ALife::eRelationTypeNeutral;

        relation_object2 = smart_cast<CSE_ALifeMonsterAbstract*>(
            ai().alife().objects().object(group->m_tpMembers.front(), true));
        if (!relation_object2)
            return ALife::eRelationTypeNeutral;
    }

    const bool human1 = smart_cast<CSE_ALifeHumanAbstract*>(relation_object1) != nullptr;
    const bool human2 = smart_cast<CSE_ALifeHumanAbstract*>(relation_object2) != nullptr;
    if (human1 || human2)
    {
        if (human1 && human2)
            return RELATION_REGISTRY().GetRelationBetween(
                smart_cast<CSE_ALifeTraderAbstract*>(relation_object1),
                smart_cast<CSE_ALifeTraderAbstract*>(relation_object2));

        return ALife::eRelationTypeEnemy;
    }

    if (!pSettings->line_exist(relation_object1->s_name, "species") ||
        !pSettings->line_exist(relation_object2->s_name, "species"))
        return ALife::eRelationTypeNeutral;

    MONSTER_COMMUNITY community1;
    MONSTER_COMMUNITY community2;
    community1.set(pSettings->r_string(relation_object1->s_name, "species"));
    community2.set(pSettings->r_string(relation_object2->s_name, "species"));

    const int relation = MONSTER_COMMUNITY::relation(community1.index(), community2.index());

    if (relation < 0)
        return (ALife::eRelationTypeEnemy);

    if (relation > 0)
        return (ALife::eRelationTypeFriend);

    return (ALife::eRelationTypeNeutral);
}
}

ALife::ERelationType CALifeCombatManager::relation_type(CSE_ALifeMonsterAbstract* tpALifeMonsterAbstract1,
    CSE_ALifeMonsterAbstract* tpALifeMonsterAbstract2) const
{
    return get_monster_relation(tpALifeMonsterAbstract1, tpALifeMonsterAbstract2);
}

void CALifeCombatManager::kill_entity(CSE_ALifeMonsterAbstract* l_tpALifeMonsterAbstract,
    const GameGraph::_GRAPH_ID& l_tGraphID, CSE_ALifeSchedulable* schedulable)
{
    if (!ALIFE_ON)
        return;

    if (!l_tpALifeMonsterAbstract || has_story_id(l_tpALifeMonsterAbstract))
        return;

    VERIFY(!l_tpALifeMonsterAbstract->g_Alive());

    append_item_vector(l_tpALifeMonsterAbstract->children, m_temp_item_vector);

    if (schedulable && schedulable != l_tpALifeMonsterAbstract)
    {
        ALifeNewsQueue::push(
            "kill",
            schedulable->base()->name_replace(),
            l_tpALifeMonsterAbstract->base()->name_replace());
    }

    const GameGraph::_GRAPH_ID old_graph_id = l_tpALifeMonsterAbstract->m_tGraphID;
    assign_death_position(l_tpALifeMonsterAbstract, l_tGraphID, schedulable);
    l_tpALifeMonsterAbstract->vfDetachAll();
    VERIFY(l_tpALifeMonsterAbstract->children.empty());

    scheduled().remove(l_tpALifeMonsterAbstract);

    if (l_tpALifeMonsterAbstract->m_tGraphID != old_graph_id)
    {
        graph().remove(l_tpALifeMonsterAbstract, old_graph_id);
        graph().add(l_tpALifeMonsterAbstract, l_tpALifeMonsterAbstract->m_tGraphID);
    }

    CSE_ALifeInventoryItem* inventory_item = smart_cast<CSE_ALifeInventoryItem*>(l_tpALifeMonsterAbstract);
    if (inventory_item)
        m_temp_item_vector.push_back(inventory_item);
}
