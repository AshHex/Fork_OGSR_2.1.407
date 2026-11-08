////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_human_object_handler.cpp
//	Created 	: 07.10.2005
//  Modified 	: 07.10.2005
//	Author		: Dmitriy Iassenev
//	Description : ALife human object handler class
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "alife_human_object_handler.h"
#include "alife_human_brain.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "alife_simulator.h"
#include "alife_graph_registry.h"
#include "alife_object_registry.h"
#include "ai_space.h"
#include "ef_storage.h"
#include "ef_primary.h"
#include "ef_pattern.h"
#include "clsid_game.h"
#include "alife_loot_queue.h"
#include "alife_runtime_switch.h"

namespace
{
constexpr u32 MAX_ITEM_FOOD_COUNT = 3;
constexpr u32 MAX_ITEM_MEDIKIT_COUNT = 3;
constexpr u32 MAX_AMMO_ATTACH_COUNT = 10;

struct CSortItemPredicate
{
    bool operator()(const CSE_ALifeInventoryItem* left, const CSE_ALifeInventoryItem* right) const
    {
        if (left->m_dwCost != right->m_dwCost)
            return left->m_dwCost > right->m_dwCost;
        return left->base()->ID < right->base()->ID;
    }
};

struct CRemoveAttachedItemsPredicate
{
    bool operator()(const CSE_ALifeInventoryItem* item) const
    {
        return !item || item->attached();
    }
};

bool is_food_item(const CSE_ALifeInventoryItem* item)
{
    if (!item)
        return false;

    if (item->base()->m_tClassID == CLSID_IITEM_FOOD || item->base()->m_tClassID == CLSID_IITEM_BOTTLE)
        return true;

    const LPCSTR section = item->base()->name();
    return section && pSettings->line_exist(section, "food_value") && pSettings->r_s32(section, "food_value") > 0;
}

bool is_equipment_item(const CSE_ALifeInventoryItem* item)
{
    if (!item)
        return false;

    if (item->base()->m_tClassID == CLSID_EQUIPMENT_SIMPLE || item->base()->m_tClassID == CLSID_EQUIPMENT_SCIENTIFIC ||
        item->base()->m_tClassID == CLSID_EQUIPMENT_STALKER || item->base()->m_tClassID == CLSID_EQUIPMENT_MILITARY ||
        item->base()->m_tClassID == CLSID_EQUIPMENT_EXO)
        return true;

    const LPCSTR section = item->base()->name();
    return section && pSettings->line_exist(section, "equipment_value") && pSettings->r_s32(section, "equipment_value") > 0;
}

bool is_medikit_item(const CSE_ALifeInventoryItem* item)
{
    if (!item)
        return false;

    if (item->base()->m_tClassID == CLSID_IITEM_MEDKIT || item->base()->m_tClassID == CLSID_IITEM_BANDAGE ||
        item->base()->m_tClassID == CLSID_IITEM_ANTIRAD)
        return true;

    const LPCSTR section = item->base()->name();
    return section && pSettings->line_exist(section, "health_value") && pSettings->r_s32(section, "health_value") > 0;
}
}

bool CALifeHumanObjectHandler::can_fit_item(const CSE_ALifeInventoryItem* inventory_item) const
{
    float mass = 0.f;
    const ALife::OBJECT_VECTOR& children = object().children;
    for (auto it = children.begin(); it != children.end(); ++it)
    {
        const CSE_ALifeDynamicObject* dynamic_object = ai().alife().objects().object(*it, true);
        const CSE_ALifeInventoryItem* item = smart_cast<const CSE_ALifeInventoryItem*>(dynamic_object);
        if (!item)
            continue;

        mass += item->m_fMass;
    }

    if (inventory_item)
    {
        mass += inventory_item->m_fMass;
    }

    if (mass > object().m_fMaxItemMass + EPS_L)
        return false;

    return true;
}

bool CALifeHumanObjectHandler::is_selected(ALife::_OBJECT_ID id, const ALife::OBJECT_VECTOR* objects) const
{
    if (objects && std::find(objects->begin(), objects->end(), id) != objects->end())
        return true;

    return std::find(object().children.begin(), object().children.end(), id) != object().children.end();
}

void CALifeHumanObjectHandler::remove_from_temp(CSE_ALifeInventoryItem* inventory_item)
{
    if (!inventory_item)
        return;

    ALife::ITEM_P_VECTOR& temp = object().alife().m_temp_item_vector;
    temp.erase(std::remove(temp.begin(), temp.end(), inventory_item), temp.end());
}

void CALifeHumanObjectHandler::attach_item(CSE_ALifeInventoryItem* inventory_item, ALife::OBJECT_VECTOR* objects)
{
    if (!inventory_item)
        return;

    CSE_ALifeDynamicObject* dynamic_object = smart_cast<CSE_ALifeDynamicObject*>(inventory_item);
    if (!dynamic_object)
        return;

    if (objects)
        object().children.push_back(inventory_item->base()->ID);
    else
        object().alife().graph().attach(object(), inventory_item, dynamic_object->m_tGraphID);
}

u16 CALifeHumanObjectHandler::get_available_ammo_count(const CSE_ALifeItemWeapon* weapon, ALife::OBJECT_VECTOR& objects)
{
    if (!weapon)
        return 0;

    if (!weapon->m_caAmmoSections)
        return u16(-1);

    u32 result = 0;
    for (ALife::OBJECT_IT it = objects.begin(); it != objects.end(); ++it)
    {
        const CSE_ALifeDynamicObject* dynamic_object = ai().alife().objects().object(*it, true);
        const CSE_ALifeItemAmmo* ammo = smart_cast<const CSE_ALifeItemAmmo*>(dynamic_object);
        if (!ammo)
            continue;

        if (strstr(weapon->m_caAmmoSections, *ammo->s_name))
            result += ammo->a_elapsed;
    }

    return u16(result);
}

u16 CALifeHumanObjectHandler::get_available_ammo_count(const CSE_ALifeItemWeapon* weapon, ALife::ITEM_P_VECTOR& items,
    ALife::OBJECT_VECTOR* objects)
{
    if (!weapon)
        return 0;

    if (!weapon->m_caAmmoSections)
        return u16(-1);

    u32 result = 0;
    u32& total_money = object().brain().m_dwTotalMoney;

    for (ALife::ITEM_P_IT it = items.begin(); it != items.end(); ++it)
    {
        CSE_ALifeItemAmmo* ammo = smart_cast<CSE_ALifeItemAmmo*>(*it);
        if (!ammo)
            continue;

        if (!strstr(weapon->m_caAmmoSections, *ammo->s_name))
            continue;

        if (ammo->m_dwCost > total_money)
            continue;

        if (objects && std::find(objects->begin(), objects->end(), ammo->base()->ID) != objects->end())
            continue;

        result += ammo->a_elapsed;
        total_money -= ammo->m_dwCost;
    }

    return u16(result);
}

void CALifeHumanObjectHandler::attach_available_ammo(CSE_ALifeItemWeapon* weapon, ALife::ITEM_P_VECTOR& items,
    ALife::OBJECT_VECTOR* objects)
{
    if (!weapon || !weapon->m_caAmmoSections)
        return;

    u32& total_money = object().brain().m_dwTotalMoney;
    const u32 safe_money = total_money;
    u32 count = 0;

    for (ALife::ITEM_P_IT it = items.begin(); it != items.end(); ++it)
    {
        CSE_ALifeItemAmmo* ammo = smart_cast<CSE_ALifeItemAmmo*>(*it);
        if (!ammo)
            continue;

        if (!strstr(weapon->m_caAmmoSections, *ammo->s_name))
            continue;

        if (ammo->m_dwCost > total_money)
            continue;

        if (objects && std::find(objects->begin(), objects->end(), ammo->base()->ID) != objects->end())
            continue;

        if (!can_take_item(ammo))
            continue;

        attach_item(ammo, objects);
        total_money -= ammo->m_dwCost;
        ++count;

        if (count >= MAX_AMMO_ATTACH_COUNT)
            break;
    }

    total_money = safe_money;
}

bool CALifeHumanObjectHandler::can_take_item(CSE_ALifeInventoryItem* inventory_item)
{
    return can_fit_item(inventory_item);
}

void CALifeHumanObjectHandler::collect_ammo_boxes()
{
    ALife::OBJECT_VECTOR& children = object().children;

    for (u32 i = 0; i < children.size(); ++i)
    {
        CSE_ALifeItemAmmo* ammo = smart_cast<CSE_ALifeItemAmmo*>(ai().alife().objects().object(children[i], true));
        if (!ammo)
            continue;

        for (u32 j = i + 1; j < children.size();)
        {
            CSE_ALifeItemAmmo* other = smart_cast<CSE_ALifeItemAmmo*>(ai().alife().objects().object(children[j], true));
            if (!other)
            {
                ++j;
                continue;
            }

            if (xr_strcmp(*ammo->s_name, *other->s_name) != 0)
            {
                ++j;
                continue;
            }

            const u32 combined = u32(ammo->a_elapsed) + u32(other->a_elapsed);
            if (combined <= ammo->m_boxSize)
            {
                ammo->a_elapsed = u16(combined);
                other->a_elapsed = 0;
                object().alife().release(other, true);
                continue;
            }

            ammo->a_elapsed = ammo->m_boxSize;
            other->a_elapsed = u16(combined - ammo->m_boxSize);
            ammo = other;
            ++j;
        }
    }

    for (u32 i = 0; i < children.size();)
    {
        CSE_ALifeItemAmmo* ammo = smart_cast<CSE_ALifeItemAmmo*>(ai().alife().objects().object(children[i], true));
        if (ammo && ammo->a_elapsed == 0)
        {
            object().alife().release(ammo, true);
            continue;
        }
        ++i;
    }
}

int CALifeHumanObjectHandler::choose_equipment(ALife::OBJECT_VECTOR* objects)
{
    CSE_ALifeInventoryItem* best_item = nullptr;
    float best_value = -flt_max;

    CEF_Storage& ef = ai().ef_storage();
    ef.alife_evaluation(true);
    ef.alife().member() = &object();

    u32& total_money = object().brain().m_dwTotalMoney;
    const u32 safe_money = total_money;

    ALife::ITEM_P_VECTOR& items = object().alife().m_temp_item_vector;
    for (ALife::ITEM_P_IT it = items.begin(); it != items.end(); ++it)
    {
        CSE_ALifeInventoryItem* item = *it;
        if (!item || item->m_dwCost > total_money)
            continue;

        if (!is_equipment_item(item))
            continue;

        if (is_selected(item->base()->ID, objects))
            continue;

        if (!can_take_item(item))
            continue;

        ef.alife().member_item() = smart_cast<CSE_ALifeObject*>(item);
        if (!ef.alife().member_item())
            continue;

        const float value = ef.m_pfItemValue->ffGetValue();
        if (value > best_value)
        {
            best_value = value;
            best_item = item;
        }
    }

    if (!best_item)
    {
        total_money = safe_money;
        ef.alife().member_item() = nullptr;
        return 0;
    }

    attach_item(best_item, objects);
    total_money -= best_item->m_dwCost;
    total_money = safe_money;
    remove_from_temp(best_item);

    ef.alife().member_item() = nullptr;
    return 1;
}

int CALifeHumanObjectHandler::choose_weapon(const ALife::EWeaponPriorityType& weapon_priority_type,
    ALife::OBJECT_VECTOR* objects)
{
    CSE_ALifeInventoryItem* best_item = nullptr;
    float best_value = -flt_max;

    CEF_Storage& ef = ai().ef_storage();
    ef.alife_evaluation(true);
    ef.alife().member() = &object();

    u32& total_money = object().brain().m_dwTotalMoney;
    const u32 safe_money = total_money;

    ALife::ITEM_P_VECTOR& items = object().alife().m_temp_item_vector;
    for (ALife::ITEM_P_IT it = items.begin(); it != items.end(); ++it)
    {
        CSE_ALifeInventoryItem* item = *it;
        if (!item || item->m_dwCost > total_money)
            continue;

        if (is_selected(item->base()->ID, objects))
            continue;

        ef.alife().member_item() = smart_cast<CSE_ALifeObject*>(item);
        if (!ef.alife().member_item())
            continue;

        const u32 type = ef.m_pfPersonalWeaponType->dwfGetWeaponType();
        float value = -1.f;

        switch (weapon_priority_type)
        {
        case ALife::eWeaponPriorityTypeKnife:
            if (type != 1)
                continue;
            value = ef.m_pfItemValue->ffGetValue();
            break;
        case ALife::eWeaponPriorityTypeSecondary:
            if (type != 5)
                continue;
            value = ef.m_pfSmallWeaponValue->ffGetValue();
            break;
        case ALife::eWeaponPriorityTypePrimary:
            if (type != 6 && type != 8 && type != 9)
                continue;
            value = ef.m_pfMainWeaponValue->ffGetValue();
            break;
        case ALife::eWeaponPriorityTypeGrenade:
            if (type != 7)
                continue;
            value = ef.m_pfItemValue->ffGetValue();
            break;
        default:
            NODEFAULT;
        }

        if (value > best_value && can_take_item(item))
        {
            best_value = value;
            best_item = item;
        }
    }

    if (!best_item)
    {
        total_money = safe_money;
        ef.alife().member_item() = nullptr;
        return 0;
    }

    CSE_ALifeItemWeapon* weapon = smart_cast<CSE_ALifeItemWeapon*>(best_item);
    const u32 old_count = object().children.size();

    if (can_take_item(best_item))
        attach_item(best_item, objects);

    total_money -= best_item->m_dwCost;
    attach_available_ammo(weapon, items, objects);
    total_money = safe_money;

    remove_from_temp(best_item);

    if (weapon)
    {
        for (ALife::ITEM_P_IT it = items.begin(); it != items.end();)
        {
            CSE_ALifeInventoryItem* item = *it;
            if (!item)
            {
                it = items.erase(it);
                continue;
            }

            if (is_selected(item->base()->ID, objects))
                it = items.erase(it);
            else
                ++it;
        }
    }

    ef.alife().member_item() = nullptr;
    return int(object().children.size() - old_count);
}

int CALifeHumanObjectHandler::choose_food(ALife::OBJECT_VECTOR* objects)
{
    ALife::ITEM_P_VECTOR& items = object().alife().m_temp_item_vector;
    u32& total_money = object().brain().m_dwTotalMoney;
    const u32 safe_money = total_money;
    u32 count = 0;

    for (ALife::ITEM_P_IT it = items.begin(); it != items.end() && count < MAX_ITEM_FOOD_COUNT;)
    {
        CSE_ALifeInventoryItem* item = *it;
        if (!item || !is_food_item(item) || item->m_dwCost > total_money || is_selected(item->base()->ID, objects) || !can_take_item(item))
        {
            ++it;
            continue;
        }

        attach_item(item, objects);
        total_money -= item->m_dwCost;
        ++count;
        it = items.erase(it);
    }

    total_money = safe_money;
    return int(count);
}

int CALifeHumanObjectHandler::choose_medikit(ALife::OBJECT_VECTOR* objects)
{
    ALife::ITEM_P_VECTOR& items = object().alife().m_temp_item_vector;
    u32& total_money = object().brain().m_dwTotalMoney;
    const u32 safe_money = total_money;
    u32 count = 0;

    for (ALife::ITEM_P_IT it = items.begin(); it != items.end() && count < MAX_ITEM_MEDIKIT_COUNT;)
    {
        CSE_ALifeInventoryItem* item = *it;
        if (!item || !is_medikit_item(item) || item->m_dwCost > total_money || is_selected(item->base()->ID, objects) || !can_take_item(item))
        {
            ++it;
            continue;
        }

        attach_item(item, objects);
        total_money -= item->m_dwCost;
        ++count;
        it = items.erase(it);
    }

    total_money = safe_money;
    return int(count);
}

int CALifeHumanObjectHandler::choose_detector(ALife::OBJECT_VECTOR* objects)
{
    CSE_ALifeItemDetector* best_item = nullptr;
    u32 best_value = 0;

    for (ALife::ITEM_P_IT it = object().alife().m_temp_item_vector.begin(); it != object().alife().m_temp_item_vector.end(); ++it)
    {
        CSE_ALifeItemDetector* detector = smart_cast<CSE_ALifeItemDetector*>(*it);
        if (!detector)
            continue;

        if (detector->m_dwCost > object().brain().m_dwTotalMoney)
            continue;
        if (is_selected(detector->base()->ID, objects))
            continue;
        if (!can_take_item(detector))
            continue;

        const u32 value = detector->ef_detector_type();
        if (!best_item || value > best_value)
        {
            best_item = detector;
            best_value = value;
        }
    }

    if (!best_item)
        return 0;

    attach_item(best_item, objects);
    remove_from_temp(best_item);
    return 1;
}

int CALifeHumanObjectHandler::choose_valuables()
{
    int count = 0;
    ALife::ITEM_P_VECTOR& items = object().alife().m_temp_item_vector;

    for (ALife::ITEM_P_IT it = items.begin(); it != items.end();)
    {
        CSE_ALifeInventoryItem* item = *it;
        if (!item || is_selected(item->base()->ID, nullptr) || !can_take_item(item))
        {
            if (!item)
                it = items.erase(it);
            else
                ++it;
            continue;
        }

        attach_item(item, nullptr);
        ++count;
        it = items.erase(it);
    }

    return count;
}

bool CALifeHumanObjectHandler::choose_fast()
{
    if (object().alife().m_temp_item_vector.empty())
        return true;

    const ALife::OBJECT_VECTOR saved_children = object().children;
    float simulated_mass = 0.f;

    for (auto it = saved_children.begin(); it != saved_children.end(); ++it)
    {
        const CSE_ALifeInventoryItem* item = smart_cast<const CSE_ALifeInventoryItem*>(ai().alife().objects().object(*it, true));
        if (!item)
            continue;
        simulated_mass += item->m_fMass;
    }

    for (ALife::ITEM_P_IT it = object().alife().m_temp_item_vector.begin(); it != object().alife().m_temp_item_vector.end(); ++it)
    {
        if (!*it)
            continue;
        simulated_mass += (*it)->m_fMass;
        if (simulated_mass > object().m_fMaxItemMass + EPS_L)
            return false;
    }

    ALife::ITEM_P_VECTOR pending = object().alife().m_temp_item_vector;
    for (ALife::ITEM_P_IT it = pending.begin(); it != pending.end(); ++it)
    {
        CSE_ALifeInventoryItem* item = *it;
        if (!item)
            continue;
        if (!is_selected(item->base()->ID, nullptr))
            attach_item(item, nullptr);
    }

    object().alife().m_temp_item_vector.clear();
    return true;
}

void CALifeHumanObjectHandler::choose_group(CSE_ALifeGroupAbstract* group_abstract)
{
    (void)group_abstract;
}

void CALifeHumanObjectHandler::detach_all(bool fictitious)
{
    while (!object().children.empty())
    {
        ALife::OBJECT_IT it = object().children.begin();
        ALife::_OBJECT_ID id = *it;
        CSE_ALifeDynamicObject* dynamic_object = ai().alife().objects().object(id, true);
        CSE_ALifeInventoryItem* item = smart_cast<CSE_ALifeInventoryItem*>(dynamic_object);

        if (!item)
        {
            object().children.erase(it);
            continue;
        }

        if (fictitious)
        {
            object().detach(item, &it);
        }
        else
        {
            object().alife().graph().detach(object(), item, object().m_tGraphID);
        }
    }
}

void CALifeHumanObjectHandler::update_weapon_ammo()
{
    CSE_ALifeItemWeapon* weapon = object().m_tpCurrentBestWeapon;
    if (!weapon)
        return;

    if (weapon->m_caAmmoSections)
    {
        u32 available = weapon->m_dwAmmoAvailable;
        for (u32 i = 0; i < object().children.size();)
        {
            CSE_ALifeItemAmmo* ammo = smart_cast<CSE_ALifeItemAmmo*>(ai().alife().objects().object(object().children[i], true));
            if (!ammo || !strstr(weapon->m_caAmmoSections, *ammo->s_name))
            {
                ++i;
                continue;
            }

            if (available >= ammo->a_elapsed)
            {
                available -= ammo->a_elapsed;
                ++i;
                continue;
            }

            if (available > 0)
            {
                ammo->a_elapsed = u16(available);
                available = 0;
                ++i;
                continue;
            }

            object().alife().release(ammo, true);
        }
    }

    weapon->m_dwAmmoAvailable = 0;
    object().m_tpCurrentBestWeapon = nullptr;
    collect_ammo_boxes();
}

void CALifeHumanObjectHandler::process_items()
{
    ALife::ITEM_P_VECTOR& temp = object().alife().m_temp_item_vector;

    if (object().g_Health() < EPS_L)
    {
        temp.clear();
        return;
    }
    temp.clear();

    float detect_probability = READ_IF_EXISTS(pSettings, r_float, object().name(), "detect_probability", 1.f);
    clamp(detect_probability, 0.f, 1.f);
    if (detect_probability <= 0.f)
        return;

    if (!ai().game_graph().valid_vertex_id(object().m_tGraphID))
        return;

    const auto& objects = ai().alife().graph().objects()[object().m_tGraphID].objects().objects();
    for (auto it = objects.begin(); it != objects.end(); ++it)
    {
        CSE_ALifeInventoryItem* item = smart_cast<CSE_ALifeInventoryItem*>(it->second);
        if (!item || !item->bfUseful() || item->base()->ID_Parent < 0xffff)
            continue;

        if (Random.randF(1.f) > detect_probability)
            continue;

        temp.push_back(item);
    }

    if (!temp.empty())
        attach_items();
}

CSE_ALifeDynamicObject* CALifeHumanObjectHandler::best_detector()
{
    CSE_ALifeDynamicObject* best = nullptr;
    u32 best_value = 0;

    if (CSE_ALifeGroupAbstract* group = smart_cast<CSE_ALifeGroupAbstract*>(&object()))
    {
        for (ALife::OBJECT_IT it = group->m_tpMembers.begin(); it != group->m_tpMembers.end(); ++it)
        {
            CSE_ALifeHumanAbstract* member = smart_cast<CSE_ALifeHumanAbstract*>(ai().alife().objects().object(*it, true));
            if (!member)
                continue;

            CSE_ALifeDynamicObject* detector = member->brain().objects().best_detector();
            CSE_ALifeItemDetector* detector_item = smart_cast<CSE_ALifeItemDetector*>(detector);
            if (!detector_item)
                continue;

            const u32 value = detector_item->ef_detector_type();
            if (!best || value > best_value)
            {
                best = detector;
                best_value = value;
            }
        }
        return best;
    }

    for (ALife::OBJECT_IT it = object().children.begin(); it != object().children.end(); ++it)
    {
        CSE_ALifeItemDetector* detector = smart_cast<CSE_ALifeItemDetector*>(ai().alife().objects().object(*it, true));
        if (!detector)
            continue;

        const u32 value = detector->ef_detector_type();
        if (!best || value > best_value)
        {
            best = detector;
            best_value = value;
        }
    }

    return best;
}

CSE_ALifeItemWeapon* CALifeHumanObjectHandler::best_weapon()
{
    object().m_tpCurrentBestWeapon = nullptr;
    u32 best_value = 0;

    for (ALife::OBJECT_IT it = object().children.begin(); it != object().children.end(); ++it)
    {
        CSE_ALifeItemWeapon* weapon = smart_cast<CSE_ALifeItemWeapon*>(ai().alife().objects().object(*it, true));
        if (!weapon)
            continue;

        weapon->m_dwAmmoAvailable = get_available_ammo_count(weapon, object().children);
        const bool no_ammo_required = !weapon->m_caAmmoSections;
        if (weapon->m_dwAmmoAvailable || no_ammo_required)
        {
            const u32 value = weapon->ef_weapon_type();
            if (!object().m_tpCurrentBestWeapon || value > best_value)
            {
                best_value = value;
                object().m_tpCurrentBestWeapon = weapon;
            }
        }
    }

    return object().m_tpCurrentBestWeapon;
}

void CALifeHumanObjectHandler::notify_picked_up_items(const ALife::OBJECT_VECTOR& previously_owned)
{
    if (!ALIFE_ON)
        return;

    for (auto id : object().children)
    {
        if (std::find(previously_owned.begin(), previously_owned.end(), id) != previously_owned.end())
            continue;

        CSE_ALifeInventoryItem* item = smart_cast<CSE_ALifeInventoryItem*>(ai().alife().objects().object(id, true));
        if (!item)
            continue;

        ALifeLootQueue::push(
            object().base()->ID, object().base()->name(),
            item->base()->ID, item->base()->name());
    }
}

void CALifeHumanObjectHandler::attach_items()
{
    R_ASSERT2(object().g_Health() >= EPS_L, "Cannot attach items to dead human");

    if (CSE_ALifeGroupAbstract* group = smart_cast<CSE_ALifeGroupAbstract*>(&object()))
    {
        choose_group(group);
        return;
    }

    const ALife::OBJECT_VECTOR previously_owned = object().children;

    if (choose_fast())
    {
        notify_picked_up_items(previously_owned);
        return;
    }

    ALife::ITEM_P_VECTOR& temp = object().alife().m_temp_item_vector;
    for (ALife::OBJECT_IT it = object().children.begin(); it != object().children.end(); ++it)
    {
        CSE_ALifeInventoryItem* item = smart_cast<CSE_ALifeInventoryItem*>(ai().alife().objects().object(*it, true));
        if (item && std::find(temp.begin(), temp.end(), item) == temp.end())
            temp.push_back(item);
    }

    detach_all(false);
    std::sort(temp.begin(), temp.end(), CSortItemPredicate());

    choose_food(nullptr);
    choose_weapon(ALife::eWeaponPriorityTypeKnife, nullptr);
    choose_weapon(ALife::eWeaponPriorityTypeSecondary, nullptr);
    choose_weapon(ALife::eWeaponPriorityTypePrimary, nullptr);
    choose_weapon(ALife::eWeaponPriorityTypeGrenade, nullptr);
    choose_medikit(nullptr);
    choose_detector(nullptr);
    choose_equipment(nullptr);
    choose_valuables();

    notify_picked_up_items(previously_owned);
}
