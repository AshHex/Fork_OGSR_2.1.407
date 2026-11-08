////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_communication_manager.cpp
//	Created 	: 03.09.2003
//  Modified 	: 14.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife communication manager
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "alife_runtime_switch.h"
#include "alife_interaction_manager.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "alife_graph_registry.h"
#include "alife_time_manager.h"
#include "alife_news_queue.h"

using namespace ALife;

CALifeInteractionManager::CALifeInteractionManager(xrServer* server, LPCSTR section)
    : CALifeCombatManager(server, section), CALifeCommunicationManager(server, section), CALifeSimulatorBase(server, section),
      m_inventory_slot_count(READ_IF_EXISTS(pSettings, r_u32, "inventory", "slots", 16u))
{
    m_temp_weapons.resize(m_inventory_slot_count);
    m_temp_marks.assign(m_inventory_slot_count, false);
}

CALifeInteractionManager::~CALifeInteractionManager() = default;

class CCheckForInteractionPredicate
{
public:
    CALifeInteractionManager* manager;
    mutable CSE_ALifeSchedulable* tpALifeSchedulable;
    mutable GameGraph::_GRAPH_ID tGraphID;
    mutable int l_iGroupIndex;
    mutable bool l_bMutualDetection;
    mutable CSE_ALifeHumanAbstract* l_tpALifeHumanAbstract;
    mutable CSE_ALifeMonsterAbstract* l_tpALifeMonsterAbstract;

    IC CCheckForInteractionPredicate(CALifeInteractionManager* manager, CSE_ALifeSchedulable* tpALifeSchedulable,
        GameGraph::_GRAPH_ID tGraphID)
        : manager(manager), tpALifeSchedulable(tpALifeSchedulable), tGraphID(tGraphID), l_iGroupIndex(-1),
          l_bMutualDetection(false), l_tpALifeHumanAbstract(nullptr), l_tpALifeMonsterAbstract(nullptr)
    {
        l_tpALifeHumanAbstract = smart_cast<CSE_ALifeHumanAbstract*>(tpALifeSchedulable);
        l_tpALifeMonsterAbstract = smart_cast<CSE_ALifeMonsterAbstract*>(tpALifeSchedulable);
        manager->vfFillCombatGroup(tpALifeSchedulable, 0);
    }

    IC bool operator()(CALifeGraphRegistry::OBJECT_REGISTRY::_iterator& I, u64 counter, bool) const
    {
        if (!(*I).second || (counter == (*I).second->m_switch_counter))
            return false;

        return !manager->m_tpaCombatGroups[0].empty();
    }

    IC void operator()(CALifeGraphRegistry::OBJECT_REGISTRY::_iterator& I, u64 counter) const
    {
        if (!(*I).second)
            return;

        (*I).second->m_switch_counter = counter;

        if (manager->m_tpaCombatGroups[0].empty())
            return;

        if ((*I).first == tpALifeSchedulable->base()->ID)
            return;

        CSE_ALifeSchedulable* other = smart_cast<CSE_ALifeSchedulable*>((*I).second);
        if (!other || !other->bfActive())
            return;

        if (CSE_ALifeHumanAbstract* human = smart_cast<CSE_ALifeHumanAbstract*>(other))
        {
            if (human->m_story_id != INVALID_STORY_ID)
                return;
        }
        else if (CSE_ALifeMonsterAbstract* monster = smart_cast<CSE_ALifeMonsterAbstract*>(other))
        {
            if (monster->m_story_id != INVALID_STORY_ID)
                return;
        }

        if (!manager->bfCheckForInteraction(tpALifeSchedulable, other, l_iGroupIndex, l_bMutualDetection))
            return;

        manager->vfFillCombatGroup(other, 1);
        if (manager->m_tpaCombatGroups[0].empty() || manager->m_tpaCombatGroups[1].empty())
            return;

        switch (manager->m_tpaCombatObjects[l_iGroupIndex]->tfGetActionType(
            manager->m_tpaCombatObjects[l_iGroupIndex ^ 1], l_iGroupIndex, l_bMutualDetection))
        {
        case eMeetActionTypeAttack:
        {
            CSE_ALifeSchedulable* combatant1 = manager->m_tpaCombatObjects[0];
            CSE_ALifeSchedulable* combatant2 = manager->m_tpaCombatObjects[1];
            if (combatant1 && combatant2)
            {
                ALifeNewsQueue::push(
                    "combat",
                    combatant1->base()->name_replace(),
                    combatant2->base()->name_replace());
            }

            ECombatResult combat_result = eCombatResultRetreat12;
            bool do_not_continue = false;

            const u32 max_iterations = _max(manager->m_dwMaxCombatIterationCount, 1u);
            for (u32 i = 0; i < max_iterations * 2u; ++i)
            {
                if (eCombatActionAttack == manager->choose_combat_action(l_iGroupIndex))
                {
                    manager->vfPerformAttackAction(l_iGroupIndex);
                    do_not_continue = false;
                }
                else
                {
                    if (do_not_continue)
                        break;

                    if (manager->bfCheckIfRetreated(l_iGroupIndex))
                    {
                        combat_result = l_iGroupIndex ? eCombatResultRetreat2 : eCombatResultRetreat1;
                        break;
                    }

                    do_not_continue = true;
                }

                l_iGroupIndex ^= 1;

                if (manager->m_tpaCombatGroups[l_iGroupIndex].empty())
                {
                    combat_result = l_iGroupIndex ? eCombatResult1Kill2 : eCombatResult2Kill1;
                    break;
                }
            }

            if (combat_result == eCombatResultRetreat1)
            {
                CSE_ALifeSchedulable* a = manager->m_tpaCombatObjects[0];
                CSE_ALifeSchedulable* b = manager->m_tpaCombatObjects[1];
                if (a && b)
                    ALifeNewsQueue::push("retreat", a->base()->name_replace(), b->base()->name_replace());
            }
            else if (combat_result == eCombatResultRetreat2)
            {
                CSE_ALifeSchedulable* a = manager->m_tpaCombatObjects[1];
                CSE_ALifeSchedulable* b = manager->m_tpaCombatObjects[0];
                if (a && b)
                    ALifeNewsQueue::push("retreat", a->base()->name_replace(), b->base()->name_replace());
            }
            else if (combat_result == eCombatResultRetreat12)
            {
                CSE_ALifeSchedulable* a = manager->m_tpaCombatObjects[0];
                CSE_ALifeSchedulable* b = manager->m_tpaCombatObjects[1];
                if (a && b)
                    ALifeNewsQueue::push("retreat", a->base()->name_replace(), b->base()->name_replace());
            }

            manager->vfFinishCombat(combat_result);
            break;
        }

        case eMeetActionTypeInteract:
            manager->vfPerformCommunication();
            break;

        case eMeetActionTypeIgnore:
            return;

        case eMeetActionSmartTerrain:
        {
            CSE_ALifeSmartZone* smart_zone = smart_cast<CSE_ALifeSmartZone*>(other);
            if (!smart_zone)
                return;

            CSE_ALifeMonsterAbstract* monster = l_tpALifeMonsterAbstract;
            if (!monster)
                return;

            smart_zone->smart_touch(monster);
            return;
        }

        default:
            return;
        }
    }
};

void CALifeInteractionManager::check_for_interaction(CSE_ALifeSchedulable* tpALifeSchedulable)
{
    if (!ALIFE_ON)
        return;

    if (!tpALifeSchedulable || !tpALifeSchedulable->bfActive())
        return;

    if (CSE_ALifeHumanAbstract* human = smart_cast<CSE_ALifeHumanAbstract*>(tpALifeSchedulable))
    {
        if (human->m_story_id != INVALID_STORY_ID)
            return;
    }
    else if (CSE_ALifeMonsterAbstract* monster = smart_cast<CSE_ALifeMonsterAbstract*>(tpALifeSchedulable))
    {
        if (monster->m_story_id != INVALID_STORY_ID)
            return;
    }

    CSE_ALifeDynamicObject* dynamic_object = smart_cast<CSE_ALifeDynamicObject*>(tpALifeSchedulable);
    if (!dynamic_object)
        return;

    const GameGraph::_GRAPH_ID graph_id = dynamic_object->m_tGraphID;
    if (!ai().game_graph().valid_vertex_id(graph_id))
        return;

    check_for_interaction(tpALifeSchedulable, graph_id);

    CGameGraph::const_iterator I, E;
    ai().game_graph().begin(graph_id, I, E);
    for (; I != E; ++I)
    {
        if (!ai().game_graph().valid_vertex_id(I->vertex_id()))
            continue;
        check_for_interaction(tpALifeSchedulable, I->vertex_id());
    }
}

void CALifeInteractionManager::check_for_interaction(CSE_ALifeSchedulable* tpALifeSchedulable,
    GameGraph::_GRAPH_ID tGraphID)
{
    if (!ALIFE_ON)
        return;

    if (!tpALifeSchedulable || !ai().game_graph().valid_vertex_id(tGraphID))
        return;

    if (CSE_ALifeHumanAbstract* human = smart_cast<CSE_ALifeHumanAbstract*>(tpALifeSchedulable))
    {
        if (human->m_story_id != INVALID_STORY_ID)
            return;
    }
    else if (CSE_ALifeMonsterAbstract* monster = smart_cast<CSE_ALifeMonsterAbstract*>(tpALifeSchedulable))
    {
        if (monster->m_story_id != INVALID_STORY_ID)
            return;
    }

    graph().iterate_objects(tGraphID, CCheckForInteractionPredicate(this, tpALifeSchedulable, tGraphID));
}
