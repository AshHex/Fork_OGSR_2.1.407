////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_human_abstract.cpp
//	Created 	: 27.10.2005
//  Modified 	: 27.10.2005
//	Author		: Dmitriy Iassenev
//	Description : ALife human abstract class
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "alife_runtime_switch.h"
#include "xrServer_objects_ALife_Monsters.h"
#include "alife_human_brain.h"
#include "alife_human_object_handler.h"
#include "alife_object_registry.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_group_registry.h"

void CSE_ALifeHumanAbstract::update()
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

bool CSE_ALifeHumanAbstract::bfPerformAttack()
{
    CSE_ALifeItemWeapon* weapon = m_tpCurrentBestWeapon;
    if (!weapon)
        return false;

    if (!weapon->m_caAmmoSections)
        return true;

    if (!weapon->m_dwAmmoAvailable)
    {
        m_tpCurrentBestWeapon = nullptr;
        return false;
    }

    if (!m_trader_flags.test(CSE_ALifeTraderAbstract::eTraderFlagInfiniteAmmo))
        --weapon->m_dwAmmoAvailable;

    if (weapon->m_dwAmmoAvailable)
        return true;

    xr_vector<ALife::_OBJECT_ID> ammo_to_release;
    for (const auto id : children)
    {
        CSE_ALifeItemAmmo* ammo = smart_cast<CSE_ALifeItemAmmo*>(ai().alife().objects().object(id, true));
        if (!ammo || !ammo->a_elapsed)
            continue;

        if (strstr(weapon->m_caAmmoSections, *ammo->base()->s_name))
            ammo_to_release.push_back(ammo->base()->ID);
    }

    for (const auto id : ammo_to_release)
    {
        CSE_ALifeDynamicObject* ammo = ai().alife().objects().object(id, true);
        if (ammo)
            alife().release(ammo, true);
    }

    m_tpCurrentBestWeapon = nullptr;
    return false;
}

ALife::EMeetActionType CSE_ALifeHumanAbstract::tfGetActionType(CSE_ALifeSchedulable* schedulable, int iGroupIndex, bool bMutualDetection)
{
    if (!ALIFE_ON)
        return ALife::eMeetActionTypeIgnore;

    if (!schedulable || m_story_id != INVALID_STORY_ID)
        return ALife::eMeetActionTypeIgnore;

    if (CSE_ALifeHumanAbstract* human = smart_cast<CSE_ALifeHumanAbstract*>(schedulable))
    {
        if (human->m_story_id != INVALID_STORY_ID)
            return ALife::eMeetActionTypeIgnore;
    }
    else if (CSE_ALifeMonsterAbstract* monster = smart_cast<CSE_ALifeMonsterAbstract*>(schedulable))
    {
        if (monster->m_story_id != INVALID_STORY_ID)
            return ALife::eMeetActionTypeIgnore;
    }

    if (ALife::eCombatTypeMonsterMonster == alife().combat_type())
    {
        CSE_ALifeMonsterAbstract* monster = smart_cast<CSE_ALifeMonsterAbstract*>(schedulable);
        if (!monster)
            return ALife::eMeetActionTypeIgnore;

        const ALife::ERelationType relation = alife().relation_type(this, monster);

        if (relation == ALife::eRelationTypeFriend)
            return ALife::eMeetActionTypeInteract;

        if (relation != ALife::eRelationTypeEnemy)
            return ALife::eMeetActionTypeIgnore;

        return (bMutualDetection || (ALife::eCombatActionAttack == alife().choose_combat_action(iGroupIndex)))
            ? ALife::eMeetActionTypeAttack
            : ALife::eMeetActionTypeIgnore;
    }

    return ALife::eMeetActionTypeAttack;
}

void CSE_ALifeHumanAbstract::vfDetachAll(bool bFictitious) { brain().objects().detach_all(bFictitious); }

void CSE_ALifeHumanAbstract::vfUpdateWeaponAmmo() { brain().objects().update_weapon_ammo(); }

void CSE_ALifeHumanAbstract::vfProcessItems() { brain().objects().process_items(); }

void CSE_ALifeHumanAbstract::vfAttachItems(ALife::ETakeType tTakeType) { brain().objects().attach_items(); }

CSE_ALifeDynamicObject* CSE_ALifeHumanAbstract::tpfGetBestDetector() { return (brain().objects().best_detector()); }

CSE_ALifeItemWeapon* CSE_ALifeHumanAbstract::tpfGetBestWeapon(ALife::EHitType& tHitType, float& fHitPower)
{
    m_tpCurrentBestWeapon = brain().objects().best_weapon();

    if (m_tpCurrentBestWeapon)
    {
        fHitPower = m_tpCurrentBestWeapon->m_fHitPower;
        tHitType = m_tpCurrentBestWeapon->m_tHitType;
        return m_tpCurrentBestWeapon;
    }

    return inherited2::tpfGetBestWeapon(tHitType, fHitPower);
}

void CSE_ALifeHumanAbstract::on_register()
{
    inherited2::on_register();
    specific_character();
}

void CSE_ALifeHumanAbstract::on_unregister() { inherited2::on_unregister(); }

void CSE_ALifeHumanAbstract::spawn_supplies()
{
    specific_character();
    inherited1::spawn_supplies();
    inherited2::spawn_supplies();
}

void CSE_ALifeHumanAbstract::add_online(const bool& update_registries)
{
    CSE_ALifeTraderAbstract::add_online(update_registries);
    brain().on_switch_online();
}

void CSE_ALifeHumanAbstract::add_offline(const xr_vector<ALife::_OBJECT_ID>& saved_children, const bool& update_registries)
{
    CSE_ALifeTraderAbstract::add_offline(saved_children, update_registries);
    brain().on_switch_offline();
}
