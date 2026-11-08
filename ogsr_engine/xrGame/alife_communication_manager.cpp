////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_communication_manager.cpp
//	Created 	: 03.09.2003
//  Modified 	: 14.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife communication manager
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "alife_runtime_switch.h"
#include "alife_communication_manager.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "alife_human_object_handler.h"
#include "alife_human_brain.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "alife_simulator.h"
#include "ai_debug.h"
#include "ef_pattern.h"
#include "alife_news_queue.h"

using namespace ALife;

namespace
{
struct CSortItemByValuePredicate
{
    IC bool operator()(const CSE_ALifeInventoryItem* a, const CSE_ALifeInventoryItem* b) const
    {
        if (a->m_dwCost != b->m_dwCost)
            return a->m_dwCost > b->m_dwCost;
        return a->base()->ID < b->base()->ID;
    }
};

struct CSortItemForSelectionPredicate
{
    IC bool operator()(const CSE_ALifeInventoryItem* a, const CSE_ALifeInventoryItem* b) const
    {
        if (a->m_dwCost != b->m_dwCost)
            return a->m_dwCost > b->m_dwCost;
        return a->base()->ID < b->base()->ID;
    }
};

struct CRemoveAttachedItemsPredicate
{
    IC bool operator()(const CSE_ALifeInventoryItem* item) const
    {
        return !item || item->attached();
    }
};

#define PUSH_STACK(a,b,c,e,f) \
    do { (e)[(f)].i1 = (a); (e)[(f)].i2 = (b); (e)[(f)].iCurrentSum = (c); ++(f); } while (false)

#define POP_STACK(a,b,c,e,f) \
    do { --(f); (a) = (e)[(f)].i1; (b) = (e)[(f)].i2; (c) = (e)[(f)].iCurrentSum; } while (false)
}

CALifeCommunicationManager::CALifeCommunicationManager(xrServer* server, LPCSTR section)
    : CALifeSimulatorBase(server, section)
{
    m_tpItems1.reserve(MAX_STACK_DEPTH);
    m_tpItems2.reserve(MAX_STACK_DEPTH);
    m_tpBlockedItems1.reserve(MAX_STACK_DEPTH);
    m_tpBlockedItems2.reserve(MAX_STACK_DEPTH);
    m_tpTrader1.reserve(MAX_STACK_DEPTH);
    m_tpTrader2.reserve(MAX_STACK_DEPTH);
    m_tpSums1.reserve(MAX_STACK_DEPTH);
    m_tpSums2.reserve(MAX_STACK_DEPTH);
}

#ifdef DEBUG
void CALifeCommunicationManager::vfPrintItems(CSE_ALifeHumanAbstract* human, ITEM_P_VECTOR& items)
{
    Msg("[LSS][TRADE] %s money=%u", human->name_replace(), human->m_dwMoney);
    for (auto* item : items)
        if (item)
            Msg("  item %s cost=%u parent=%u", item->base()->name_replace(), item->m_dwCost, item->base()->ID_Parent);
}

void CALifeCommunicationManager::vfPrintItems(CSE_ALifeHumanAbstract* human)
{
    Msg("[LSS][TRADE] %s money=%u", human->name_replace(), human->m_dwMoney);
    for (auto id : human->children)
    {
        auto* object = objects().object(id, true);
        if (object)
            Msg("  child %s[%u]", object->name_replace(), object->ID);
    }
}
#endif

u32 CALifeCommunicationManager::dwfComputeItemCost(ITEM_P_VECTOR& items)
{
    u64 cost = 0;
    for (auto* item : items)
        if (item)
            cost += item->m_dwCost;
    return cost > u32(-1) ? u32(-1) : u32(cost);
}

void CALifeCommunicationManager::vfRunFunctionByIndex(
    CSE_ALifeHumanAbstract* human,
    OBJECT_VECTOR& blocked_items,
    ITEM_P_VECTOR& items,
    int function_index,
    int& item_count)
{
    std::sort(m_temp_item_vector.begin(), m_temp_item_vector.end(), CSortItemForSelectionPredicate());

    switch (function_index)
    {
    case 0:
        item_count = human->brain().objects().choose_food(&blocked_items);
        break;
    case 1:
        item_count = human->brain().objects().choose_weapon(eWeaponPriorityTypeKnife, &blocked_items);
        break;
    case 2:
        item_count = human->brain().objects().choose_weapon(eWeaponPriorityTypeSecondary, &blocked_items);
        break;
    case 3:
        item_count = human->brain().objects().choose_weapon(eWeaponPriorityTypePrimary, &blocked_items);
        break;
    case 4:
        item_count = human->brain().objects().choose_weapon(eWeaponPriorityTypeGrenade, &blocked_items);
        break;
    case 5:
        item_count = human->brain().objects().choose_medikit(&blocked_items);
        break;
    case 6:
        item_count = human->brain().objects().choose_detector(&blocked_items);
        break;
    case 7:
        item_count = human->brain().objects().choose_equipment(&blocked_items);
        break;
    default:
        item_count = 0;
        break;
    }

    (void)items;
}

void CALifeCommunicationManager::vfAssignItemParents(CSE_ALifeHumanAbstract* human, int item_count)
{
    if (item_count <= 0 || item_count > int(human->children.size()))
        return;

    const auto begin = human->children.end() - item_count;
    std::vector<_OBJECT_ID> selected(begin, human->children.end());
    human->children.resize(human->children.size() - item_count);

    for (auto id : selected)
    {
        auto* item = smart_cast<CSE_ALifeInventoryItem*>(objects().object(id, true));
        if (!item)
            continue;

        auto duplicate = std::find(human->children.begin(), human->children.end(), item->base()->ID);
        if (duplicate != human->children.end())
            human->children.erase(duplicate);

        human->attach(item, true, true);
        if (human->brain().m_dwTotalMoney >= item->m_dwCost)
            human->brain().m_dwTotalMoney -= item->m_dwCost;
        else
            human->brain().m_dwTotalMoney = 0;
    }
}

void CALifeCommunicationManager::vfAttachOwnerItems(
    CSE_ALifeHumanAbstract* human,
    ITEM_P_VECTOR& items,
    ITEM_P_VECTOR& own_items)
{
    if (!human)
        return;

    for (auto* item : items)
    {
        if (!item || !std::binary_search(own_items.begin(), own_items.end(), item))
            continue;

        const ALife::_OBJECT_ID id = item->base()->ID;
        auto existing = std::find(human->children.begin(), human->children.end(), id);

        if (existing != human->children.end())
        {
#ifdef DEBUG
            Msg("[LSS][TRADE] vfAttachOwnerItems: item %u already in human %u, skip attach", id, human->ID);
#endif
            item->base()->ID_Parent = human->ID;
            continue;
        }

        human->attach(item, true, true);
    }
}

int CALifeCommunicationManager::ifComputeBalance(CSE_ALifeHumanAbstract* human, ITEM_P_VECTOR& items)
{
    int debt = 0;
    for (auto* item : items)
    {
        if (!item)
            continue;
        if (std::find(human->children.begin(), human->children.end(), item->base()->ID) != human->children.end())
            debt -= int(item->m_dwCost);
    }
    return debt;
}

void CALifeCommunicationManager::vfRestoreItems(CSE_ALifeHumanAbstract* human, ITEM_P_VECTOR& items)
{
    for (auto* item : items)
    {
        if (!item)
            continue;
        if (std::find(human->children.begin(), human->children.end(), item->base()->ID) == human->children.end())
            human->attach(item, true, true);
    }
}

void CALifeCommunicationManager::vfFillTraderVector(CSE_ALifeHumanAbstract* human, int item_count, ITEM_P_VECTOR& items)
{
    items.clear();
    if (item_count <= 0 || item_count > int(human->children.size()))
        return;

    for (auto it = human->children.end() - item_count; it != human->children.end(); ++it)
    {
        if (auto* item = smart_cast<CSE_ALifeInventoryItem*>(objects().object(*it, true)))
            items.push_back(item);
    }
}

void CALifeCommunicationManager::vfGenerateSums(ITEM_P_VECTOR& trader, INT_VECTOR& sums)
{
    int stack_pointer = 0;
    sums.clear();
    sums.push_back(0);

    for (int i = 0, n = int(trader.size()); i < n; ++i)
    {
        PUSH_STACK(i + 1, n - 1, 0, m_tpStack1, stack_pointer);
        while (stack_pointer)
        {
            int i1, i2, current_sum;
            POP_STACK(i1, i2, current_sum, m_tpStack1, stack_pointer);

            if (!i1)
            {
                auto pos = std::lower_bound(sums.begin(), sums.end(), current_sum);
                if (pos == sums.end() || *pos != current_sum)
                {
                    sums.insert(pos, current_sum);
                    if (sums.size() >= SUM_COUNT_THRESHOLD)
                        return;
                }
                continue;
            }

            for (int index = i2; index >= 0; --index)
                PUSH_STACK(i1 - 1, index - 1, current_sum + int(trader[index]->m_dwCost), m_tpStack1, stack_pointer);
        }
    }
}

bool CALifeCommunicationManager::bfGetItemIndexes(
    ITEM_P_VECTOR& trader,
    int target_sum,
    INT_VECTOR& indexes,
    SSumStackCell* stack,
    int start_index,
    int stack_pointer)
{
    const int n = int(trader.size());
    for (int j = start_index; j < n; ++j)
    {
        PUSH_STACK(j + 1, n - 1, 0, stack, stack_pointer);
        indexes.resize(j + 2);

        while (stack_pointer)
        {
            int i1, i2, current_sum;
            POP_STACK(i1, i2, current_sum, stack, stack_pointer);
            indexes[i1] = i2 + 1;

            if (!i1)
            {
                if (current_sum == target_sum)
                {
                    indexes.resize(j + 1);
                    return true;
                }
                continue;
            }

            if (current_sum >= target_sum)
                continue;

            for (int index = i2; index >= 0; --index)
                PUSH_STACK(i1 - 1, index - 1, current_sum + int(trader[index]->m_dwCost), stack, stack_pointer);
        }
    }
    return false;
}

bool CALifeCommunicationManager::bfCheckForInventoryCapacity(
    CSE_ALifeHumanAbstract* human1,
    ITEM_P_VECTOR& trader1,
    INT_VECTOR& indexes1,
    CSE_ALifeHumanAbstract* human2,
    ITEM_P_VECTOR& trader2,
    INT_VECTOR& indexes2)
{
    const auto original1 = human1->children;
    const auto original2 = human2->children;

    for (int index : indexes2)
    {
        if (index < 0 || index >= int(trader2.size()) || !trader2[index])
            continue;

        const ALife::_OBJECT_ID id = trader2[index]->base()->ID;
        human1->children.push_back(id);

        auto it = std::find(human2->children.begin(), human2->children.end(), id);
        if (it != human2->children.end())
            human2->children.erase(it);
    }

    for (int index : indexes1)
    {
        if (index < 0 || index >= int(trader1.size()) || !trader1[index])
            continue;

        const ALife::_OBJECT_ID id = trader1[index]->base()->ID;
        human2->children.push_back(id);

        auto it = std::find(human1->children.begin(), human1->children.end(), id);
        if (it != human1->children.end())
            human1->children.erase(it);
    }

    const bool result =
        human1->brain().objects().can_take_item(nullptr) &&
        human2->brain().objects().can_take_item(nullptr);

    if (!result)
    {
        human1->children = original1;
        human2->children = original2;
    }

    return result;
}

bool CALifeCommunicationManager::bfCheckForInventoryCapacity(
    CSE_ALifeHumanAbstract* human1,
    ITEM_P_VECTOR& trader1,
    int sum1,
    int money1,
    CSE_ALifeHumanAbstract* human2,
    ITEM_P_VECTOR& trader2,
    int sum2,
    int money2,
    int balance)
{
    const auto original1 = human1->children;
    const auto original2 = human2->children;

    INT_VECTOR indexes1, indexes2;
    int start1 = 0, stack1 = 0;
    int start2 = 0, stack2 = 0;

    constexpr int MAX_CAPACITY_RETRY_ATTEMPTS = 4;

    for (int attempt1 = 0; attempt1 < MAX_CAPACITY_RETRY_ATTEMPTS; ++attempt1)
    {
        indexes1.clear();
        if (sum1 && !bfGetItemIndexes(trader1, sum1, indexes1, m_tpStack1, start1, stack1))
            return false;

        for (int attempt2 = 0; attempt2 < MAX_CAPACITY_RETRY_ATTEMPTS; ++attempt2)
        {
            indexes2.clear();
            if (sum2 && !bfGetItemIndexes(trader2, sum2, indexes2, m_tpStack2, start2, stack2))
                return false;

            if (!bfCheckForInventoryCapacity(human1, trader1, indexes1, human2, trader2, indexes2))
                continue;

            if (sum1 < balance + sum2)
            {
                const int payment = balance + sum2 - sum1;
                if (payment < 0 || u32(payment) > human1->m_dwMoney)
                {
                    human1->children = original1;
                    human2->children = original2;
                    return false;
                }
                human1->m_dwMoney -= u32(payment);
                human2->m_dwMoney += u32(payment);
            }
            else if (sum1 > balance + sum2)
            {
                const int payment = sum1 - balance - sum2;
                if (payment < 0 || u32(payment) > human2->m_dwMoney)
                {
                    human1->children = original1;
                    human2->children = original2;
                    return false;
                }
                human1->m_dwMoney += u32(payment);
                human2->m_dwMoney -= u32(payment);
            }
            (void)money1;
            (void)money2;
            return true;
        }
    }

    human1->children = original1;
    human2->children = original2;
    return false;
}

bool CALifeCommunicationManager::bfCheckForTrade(
    CSE_ALifeHumanAbstract* human1,
    ITEM_P_VECTOR& trader1,
    INT_VECTOR& sums1,
    int money1,
    CSE_ALifeHumanAbstract* human2,
    ITEM_P_VECTOR& trader2,
    INT_VECTOR& sums2,
    int money2,
    int balance)
{
    for (int sum1 : sums1)
    {
        if (sum1 + money1 < balance)
            continue;

        if (sum1 < balance)
        {
            if (bfCheckForInventoryCapacity(human1, trader1, sum1, money1, human2, trader2, 0, money2, balance))
                return true;
            continue;
        }

        for (int sum2 : sums2)
        {
            if (sum1 >= sum2 + balance)
            {
                if (sum1 - sum2 - balance <= money2 &&
                    bfCheckForInventoryCapacity(human1, trader1, sum1, money1, human2, trader2, sum2, money2, balance))
                    return true;
            }
            else if (sum1 + money1 >= sum2 + balance &&
                     bfCheckForInventoryCapacity(human1, trader1, sum1, money1, human2, trader2, sum2, money2, balance))
            {
                return true;
            }
            else
                break;
        }
    }
    return false;
}

bool CALifeCommunicationManager::bfCheckIfCanNullTradersBalance(
    CSE_ALifeHumanAbstract* human1,
    CSE_ALifeHumanAbstract* human2,
    int item_count1,
    int item_count2,
    int balance)
{
    if (!balance)
        return true;

    if (balance < 0 && human1->m_dwMoney >= u32(-balance))
    {
        human1->m_dwMoney -= u32(-balance);
        human2->m_dwMoney += u32(-balance);
        return true;
    }

    if (balance > 0 && human2->m_dwMoney >= u32(balance))
    {
        human1->m_dwMoney += u32(balance);
        human2->m_dwMoney -= u32(balance);
        return true;
    }

    vfFillTraderVector(human1, item_count1, m_tpTrader1);
    vfFillTraderVector(human2, item_count2, m_tpTrader2);

    std::sort(m_tpTrader1.begin(), m_tpTrader1.end(), CSortItemByValuePredicate());
    std::sort(m_tpTrader2.begin(), m_tpTrader2.end(), CSortItemByValuePredicate());

    vfGenerateSums(m_tpTrader1, m_tpSums1);
    vfGenerateSums(m_tpTrader2, m_tpSums2);

    if (balance < 0)
        return bfCheckForTrade(human1, m_tpTrader1, m_tpSums1, human1->m_dwMoney, human2, m_tpTrader2, m_tpSums2, human2->m_dwMoney, -balance);

    return bfCheckForTrade(human2, m_tpTrader2, m_tpSums2, human2->m_dwMoney, human1, m_tpTrader1, m_tpSums1, human1->m_dwMoney, balance);
}

void CALifeCommunicationManager::vfAppendBlockedItems(
    CSE_ALifeHumanAbstract* human,
    OBJECT_VECTOR& blocked1,
    OBJECT_VECTOR& blocked2,
    int item_count)
{
    if (item_count <= 0 || item_count > int(human->children.size()))
        return;

    for (auto it = human->children.end() - item_count; it != human->children.end(); ++it)
        if (std::find(blocked2.begin(), blocked2.end(), *it) == blocked2.end())
            blocked1.push_back(*it);
}

void CALifeCommunicationManager::vfPerformTrading(CSE_ALifeHumanAbstract* human1, CSE_ALifeHumanAbstract* human2)
{
    if (!ALIFE_ON)
        return;

    if (!human1 || !human2 || human1->m_story_id != INVALID_STORY_ID || human2->m_story_id != INVALID_STORY_ID)
        return;

    m_tpItems1.clear();
    m_tpItems2.clear();

    const ALife::OBJECT_VECTOR initial_children1 = human1->children;
    const ALife::OBJECT_VECTOR initial_children2 = human2->children;
    const u32 initial_money1 = human1->m_dwMoney;
    const u32 initial_money2 = human2->m_dwMoney;

    append_item_vector(human1->children, m_tpItems1);
    append_item_vector(human2->children, m_tpItems2);

    human1->brain().m_dwTotalMoney = dwfComputeItemCost(m_tpItems1) + human1->m_dwMoney;
    human2->brain().m_dwTotalMoney = dwfComputeItemCost(m_tpItems2) + human2->m_dwMoney;

    if (!human1->brain().m_dwTotalMoney || !human2->brain().m_dwTotalMoney)
    {
        human1->brain().m_dwTotalMoney = u32(-1);
        human2->brain().m_dwTotalMoney = u32(-1);
        return;
    }

    m_temp_item_vector = m_tpItems1;
    m_temp_item_vector.insert(m_temp_item_vector.end(), m_tpItems2.begin(), m_tpItems2.end());

    std::sort(m_tpItems1.begin(), m_tpItems1.end());
    std::sort(m_tpItems2.begin(), m_tpItems2.end());

    human1->vfDetachAll(true);
    human2->vfDetachAll(true);

    for (int j = 0, mode = 0; j < 8; ++j)
    {
        if (m_temp_item_vector.empty() ||
            !(human1->brain().m_dwTotalMoney + human2->brain().m_dwTotalMoney))
            break;

        int count1 = 0, count2 = 0;
        switch (mode)
        {
        case 0:
            vfRunFunctionByIndex(human1, m_tpBlockedItems1, m_tpItems1, j, count1);
            vfRunFunctionByIndex(human2, m_tpBlockedItems2, m_tpItems2, j, count2);
            break;
        case 1:
            vfRunFunctionByIndex(human1, m_tpBlockedItems1, m_tpItems1, j, count1);
            break;
        case 2:
            vfRunFunctionByIndex(human2, m_tpBlockedItems2, m_tpItems2, j, count2);
            break;
        case 3:
            vfRunFunctionByIndex(human1, m_tpBlockedItems1, m_tpItems1, j, count1);
            m_tpBlockedItems2.clear();
            if (count1 > 0)
                m_tpBlockedItems2.insert(m_tpBlockedItems2.end(), human1->children.end() - count1, human1->children.end());
            vfRunFunctionByIndex(human2, m_tpBlockedItems2, m_tpItems2, j, count2);
            m_tpBlockedItems1.clear();
            if (count2 > 0)
                m_tpBlockedItems1.insert(m_tpBlockedItems1.end(), human2->children.end() - count2, human2->children.end());
            if (count1 > 0 && count1 <= int(human1->children.size()))
                human1->children.resize(human1->children.size() - count1);
            vfRunFunctionByIndex(human1, m_tpBlockedItems1, m_tpItems1, j, count1);
            break;
        }

        m_tpBlockedItems1.clear();
        m_tpBlockedItems2.clear();

        if (count1 > 0 && count2 > 0)
        {
            auto begin = human1->children.end() - count1;
            for (auto it = begin; it != human1->children.end(); ++it)
            {
                if (std::find(human2->children.end() - count2, human2->children.end(), *it) != human2->children.end())
                    m_tpBlockedItems1.push_back(*it);
            }
        }

        if (m_tpBlockedItems1.empty() && m_tpBlockedItems2.empty())
        {
            if (count1 > 0) vfAssignItemParents(human1, count1);
            if (count2 > 0) vfAssignItemParents(human2, count2);

            if (count1 + count2 > 0)
            {
                auto it = std::remove_if(m_temp_item_vector.begin(), m_temp_item_vector.end(), CRemoveAttachedItemsPredicate());
                m_temp_item_vector.erase(it, m_temp_item_vector.end());
            }
            mode = 0;
        }
        else
        {
            if (!m_tpBlockedItems1.empty() && !m_tpBlockedItems2.empty())
            {
                vfAppendBlockedItems(human1, m_tpBlockedItems1, m_tpBlockedItems2, count1);
                vfAppendBlockedItems(human2, m_tpBlockedItems2, m_tpBlockedItems1, count2);
                if (count1 > 0 && count1 <= int(human1->children.size())) human1->children.resize(human1->children.size() - count1);
                if (count2 > 0 && count2 <= int(human2->children.size())) human2->children.resize(human2->children.size() - count2);
                mode = 3;
            }
            else if (!m_tpBlockedItems1.empty())
            {
                if (count1 > 0 && count1 <= int(human1->children.size())) human1->children.resize(human1->children.size() - count1);
                if (count2 > 0) vfAssignItemParents(human2, count2);
                auto it = std::remove_if(m_temp_item_vector.begin(), m_temp_item_vector.end(), CRemoveAttachedItemsPredicate());
                m_temp_item_vector.erase(it, m_temp_item_vector.end());
                mode = 1;
            }
            else
            {
                if (count2 > 0 && count2 <= int(human2->children.size())) human2->children.resize(human2->children.size() - count2);
                if (count1 > 0) vfAssignItemParents(human1, count1);
                auto it = std::remove_if(m_temp_item_vector.begin(), m_temp_item_vector.end(), CRemoveAttachedItemsPredicate());
                m_temp_item_vector.erase(it, m_temp_item_vector.end());
                mode = 2;
            }
            --j;
        }
    }

    const int initial1 = int(human1->children.size());
    const int initial2 = int(human2->children.size());

    vfAttachOwnerItems(human1, m_temp_item_vector, m_tpItems1);
    vfAttachOwnerItems(human2, m_temp_item_vector, m_tpItems2);

    const int balance = ifComputeBalance(human1, m_tpItems2) - ifComputeBalance(human2, m_tpItems1);

    if (!bfCheckIfCanNullTradersBalance(human1, human2, int(human1->children.size()) - initial1, int(human2->children.size()) - initial2, balance))
    {
        vfRestoreItems(human1, m_tpItems1);
        vfRestoreItems(human2, m_tpItems2);
    }

    vfAttachGatheredItems(human1, human2, m_tpBlockedItems1);
    vfAttachGatheredItems(human2, human1, m_tpBlockedItems2);

    if (initial_children1 != human1->children ||
        initial_children2 != human2->children ||
        initial_money1 != human1->m_dwMoney ||
        initial_money2 != human2->m_dwMoney)
    {
        ALifeNewsQueue::push(
            "trade",
            human1->base()->name_replace(),
            human2->base()->name_replace());
    }

    human1->brain().m_dwTotalMoney = u32(-1);
    human2->brain().m_dwTotalMoney = u32(-1);
}

void CALifeCommunicationManager::vfAttachGatheredItems(
    CSE_ALifeTraderAbstract* trader1,
    CSE_ALifeTraderAbstract* trader2,
    OBJECT_VECTOR& object_vector)
{
    object_vector.clear();
    if (!trader1 || !trader1->base())
        return;

    object_vector.insert(object_vector.end(), trader1->base()->children.begin(), trader1->base()->children.end());
    trader1->base()->children.clear();

    CSE_ALifeDynamicObject* dynamic_trader = smart_cast<CSE_ALifeDynamicObject*>(trader1);
    if (!dynamic_trader)
        return;

    std::sort(object_vector.begin(), object_vector.end());
    object_vector.erase(std::unique(object_vector.begin(), object_vector.end()), object_vector.end());

    for (auto id : object_vector)
    {
        auto* item = smart_cast<CSE_ALifeInventoryItem*>(objects().object(id, true));
        if (!item)
            continue;

        if (std::find(dynamic_trader->children.begin(), dynamic_trader->children.end(), id) != dynamic_trader->children.end())
            continue;

        dynamic_trader->attach(item, true, true);
    }

    (void)trader2;
}

void CALifeCommunicationManager::vfPerformCommunication()
{
    if (!ALIFE_ON)
        return;

    for (auto* first : m_tpaCombatGroups[0])
    {
        auto* human1 = smart_cast<CSE_ALifeHumanAbstract*>(first);
        if (!human1 || human1->m_story_id != INVALID_STORY_ID)
            continue;

        for (auto* second : m_tpaCombatGroups[1])
        {
            auto* human2 = smart_cast<CSE_ALifeHumanAbstract*>(second);
            if (!human2 || human2->m_story_id != INVALID_STORY_ID)
                continue;
            if (!human1->children.empty() || !human2->children.empty())
                vfPerformTrading(human1, human2);
        }
    }
}

void CALifeCommunicationManager::communicate_with_customer(CSE_ALifeHumanAbstract* human, CSE_ALifeTrader* trader)
{
    if (!human || !trader || human->m_story_id != INVALID_STORY_ID)
        return;

    auto* trader_object = smart_cast<CSE_ALifeDynamicObject*>(trader);
    if (!trader_object)
        return;

    auto* group = smart_cast<CSE_ALifeGroupAbstract*>(human);
    if (group)
    {
        for (auto id : group->m_tpMembers)
        {
            auto* member = smart_cast<CSE_ALifeHumanAbstract*>(objects().object(id, true));
            if (member)
                communicate_with_customer(member, trader);
        }
        return;
    }

    u64 total_money = human->m_dwMoney;
    CSE_ALifeItemPDA* original_pda = nullptr;

    for (auto it = human->children.begin(); it != human->children.end(); )
    {
        auto* item = smart_cast<CSE_ALifeInventoryItem*>(objects().object(*it, true));
        if (!item)
        {
            ++it;
            continue;
        }

        auto* pda = smart_cast<CSE_ALifeItemPDA*>(item);
        if (pda && pda->m_original_owner == human->ID)
            original_pda = pda;

        const u32 price = item->m_dwCost;
        human->detach(item, nullptr, true, false);
        trader_object->attach(item, true);
        total_money += price;
        if (trader->m_dwMoney >= price)
            trader->m_dwMoney -= price;
        else
            trader->m_dwMoney = 0;

        it = human->children.begin();
    }

    human->children.clear();
    human->m_dwMoney = total_money > u32(-1) ? u32(-1) : u32(total_money);

    if (original_pda)
    {
        trader_object->detach(original_pda);
        human->attach(original_pda, true);
    }

    CSE_ALifeCreatureActor* actor = ai().alife().graph().actor();
    if (!actor || human->ID != actor->ID)
    {
        ALifeNewsQueue::push(
            "trade",
            human->base()->name_replace(),
            trader->base()->name_replace());
    }
}
