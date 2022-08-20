/*
 * BattleAI.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "BattleExchangeVariant.h"
#include "../../lib/CStack.h"

int64_t BattleExchangeVariant::trackAttack(const AttackPossibility & ap, HypotheticBattle * state)
{
	auto affectedUnits = ap.affectedUnits;

	affectedUnits.push_back(ap.attackerState);

	for(auto affectedUnit : affectedUnits)
	{
		auto unitToUpdate = state->getForUpdate(affectedUnit->unitId());

		unitToUpdate->health = affectedUnit->health;
		unitToUpdate->shots = affectedUnit->shots;
		unitToUpdate->counterAttacks = affectedUnit->counterAttacks;
		unitToUpdate->movedThisRound = affectedUnit->movedThisRound;
	}

	auto attackValue = ap.attackValue();

	dpsScore += attackValue;

	logAi->trace(
		"%s -> %s, ap attack, %s, dps: %d, score: %d",
		ap.attack.attacker->getDescription(),
		ap.attack.defender->getDescription(),
		ap.attack.shooting ? "shot" : "mellee",
		ap.damageDealt,
		attackValue);

	return attackValue;
}

int64_t BattleExchangeVariant::trackAttack(
	std::shared_ptr<StackWithBonuses> attacker,
	std::shared_ptr<StackWithBonuses> defender,
	bool shooting,
	bool isOurAttack,
	std::shared_ptr<CBattleInfoCallback> cb,
	bool evaluateOnly)
{
	const std::string cachingStringBlocksRetaliation = "type_BLOCKS_RETALIATION";
	static const auto selectorBlocksRetaliation = Selector::type()(Bonus::BLOCKS_RETALIATION);
	const bool counterAttacksBlocked = attacker->hasBonus(selectorBlocksRetaliation, cachingStringBlocksRetaliation);

	TDmgRange retalitation;
	BattleAttackInfo bai(attacker.get(), defender.get(), shooting);
	auto attack = cb->battleEstimateDamage(bai, &retalitation);
	int64_t attackDamage = (attack.first + attack.second) / 2;
	int64_t defenderDpsReduce = calculateDpsReduce(attacker.get(), defender.get(), attackDamage, cb);
	int64_t attackerDpsReduce = 0;

	if(!evaluateOnly)
	{
		logAi->trace(
			"%s -> %s, normal attack, %s, dps: %d, %d",
			attacker->getDescription(),
			defender->getDescription(),
			shooting ? "shot" : "mellee",
			attackDamage,
			defenderDpsReduce);

		if(isOurAttack)
		{
			dpsScore += defenderDpsReduce;
			attackerValue[attacker->unitId()].value += defenderDpsReduce;
		}
		else
			dpsScore -= defenderDpsReduce;

		defender->damage(attackDamage);
		attacker->afterAttack(shooting, false);
	}

	if(defender->alive() && defender->ableToRetaliate() && !counterAttacksBlocked && !shooting)
	{
		if(retalitation.second != 0)
		{
			auto retalitationDamage = (retalitation.first + retalitation.second) / 2;
			attackerDpsReduce = calculateDpsReduce(defender.get(), attacker.get(), retalitationDamage, cb);

			if(!evaluateOnly)
			{
				logAi->trace(
					"%s -> %s, retalitation, dps: %d, %d",
					defender->getDescription(),
					attacker->getDescription(),
					retalitationDamage,
					attackerDpsReduce);

				if(isOurAttack)
				{
					dpsScore -= attackerDpsReduce;
					attackerValue[attacker->unitId()].isRetalitated = true;
				}
				else
				{
					dpsScore += attackerDpsReduce;
					attackerValue[defender->unitId()].value += attackerDpsReduce;
				}

				attacker->damage(retalitationDamage);
				defender->afterAttack(false, true);
			}
		}
	}

	auto score = defenderDpsReduce - attackerDpsReduce;

	if(!score)
	{
		logAi->trace("Zero %d %d", defenderDpsReduce, attackerDpsReduce);
	}

	return score;
}

int64_t BattleExchangeVariant::calculateDpsReduce(
	const battle::Unit * attacker,
	const battle::Unit * defender,
	uint64_t damageDealt,
	std::shared_ptr<CBattleInfoCallback> cb) const
{
	vstd::amin(damageDealt, defender->getAvailableHealth());

	auto enemyDamageBeforeAttack = cb->battleEstimateDamage(BattleAttackInfo(defender, attacker, defender->canShoot()));
	auto enemiesKilled = damageDealt / defender->MaxHealth() + (damageDealt % defender->MaxHealth() >= defender->getFirstHPleft() ? 1 : 0);
	auto enemyDps = (enemyDamageBeforeAttack.first + enemyDamageBeforeAttack.second) / 2;

	return (int64_t)(enemyDps * enemiesKilled / (double)defender->getCount()
		+ enemyDps / (double)defender->getCount() * ((damageDealt - defender->getFirstHPleft()) % defender->MaxHealth()) / defender->MaxHealth());
};

EvaluationResult BattleExchangeEvaluator::findBestTarget(const battle::Unit * activeStack, PotentialTargets & targets, HypotheticBattle & hb)
{
	EvaluationResult result(targets.bestAction());

	updateReachabilityMap(hb);

	for(auto & ap : targets.possibleAttacks)
	{
		int64_t score = calculateExchange(ap);

		if(score > result.score)
		{
			result.score = score;
			result.bestAttack = ap;
		}
	}

	if(!activeStack->waited())
	{
		logAi->trace("Evaluating waited attack for %s", activeStack->getDescription());

		hb.getForUpdate(activeStack->unitId())->waiting = true;
		hb.getForUpdate(activeStack->unitId())->waitedThisTurn = true;

		updateReachabilityMap(hb);

		for(auto & ap : targets.possibleAttacks)
		{
			int64_t score = calculateExchange(ap);

			if(score > result.score)
			{
				result.score = score;
				result.bestAttack = ap;
				result.wait = true;
			}
		}
	}

	return result;
}

std::vector<const battle::Unit *> BattleExchangeEvaluator::getExchangeUnits(
	const AttackPossibility & ap)
{
	auto hexes = ap.attack.defender->getHexes();

	if(!ap.attack.shooting) hexes.push_back(ap.from);

	std::vector<const battle::Unit *> exchangeUnits;
	std::vector<const battle::Unit *> allReachableUnits;

	for(auto hex : hexes)
	{
		vstd::concatenate(allReachableUnits, reachabilityMap[hex]);
	}

	vstd::removeDuplicates(allReachableUnits);

	if(allReachableUnits.size() < 2)
	{
		logAi->trace("Reachability map contains only %d stacks", allReachableUnits.size());

		return exchangeUnits;
	}

	for(int turn = 0; turn < turnOrder.size(); turn++)
	{
		for(auto unit : turnOrder[turn])
		{
			if(vstd::contains(allReachableUnits, unit))
				exchangeUnits.push_back(unit);
		}
	}

	return exchangeUnits;
}

int64_t BattleExchangeEvaluator::calculateExchange(const AttackPossibility & ap)
{
	logAi->trace("Battle exchange at %d", ap.attack.shooting ? ap.dest : ap.from);

	std::vector<const battle::Unit *> ourStacks;
	std::vector<const battle::Unit *> enemyStacks;

	enemyStacks.push_back(ap.attack.defender);

	std::vector<const battle::Unit *> exchangeUnits = getExchangeUnits(ap);

	if(exchangeUnits.empty())
	{
		return 0;
	}

	HypotheticBattle exchangeBattle(env.get(), cb);
	BattleExchangeVariant v;
	auto melleeAttackers = ourStacks;

	vstd::removeDuplicates(melleeAttackers);
	vstd::erase_if(melleeAttackers, [&](const battle::Unit * u) -> bool
		{
			return !cb->battleCanShoot(u);
		});

	for(auto unit : exchangeUnits)
	{
		bool isOur = cb->battleMatchOwner(ap.attack.attacker, unit, true);
		auto & attackerQueue = isOur ? ourStacks : enemyStacks;
		auto & oppositeQueue = isOur ? enemyStacks : ourStacks;

		if(!vstd::contains(attackerQueue, unit))
		{
			attackerQueue.push_back(unit);
		}
	}

	bool canUseAp = true;

	for(auto activeUnit : exchangeUnits)
	{
		bool isOur = cb->battleMatchOwner(ap.attack.attacker, activeUnit, true);
		battle::Units & attackerQueue = isOur ? ourStacks : enemyStacks;
		battle::Units & oppositeQueue = isOur ? enemyStacks : ourStacks;

		auto attacker = exchangeBattle.getForUpdate(activeUnit->unitId());

		if(!attacker->alive() || oppositeQueue.empty())
		{
			logAi->trace(
				"Attacker [%s] dead(%d) or opposite queue empty(%d)",
				attacker->getDescription(),
				attacker->alive() ? 0 : 1,
				oppositeQueue.size());

			continue;
		}

		auto targetUnit = ap.attack.defender;

		if(!isOur || !exchangeBattle.getForUpdate(targetUnit->unitId())->alive())
		{
			targetUnit = *vstd::maxElementByFun(oppositeQueue, [&](const battle::Unit * u) -> int64_t
				{
					auto stackWithBonuses = exchangeBattle.getForUpdate(u->unitId());
					auto score = v.trackAttack(
						attacker,
						stackWithBonuses,
						exchangeBattle.battleCanShoot(stackWithBonuses.get()),
						isOur,
						cb,
						true);

					logAi->trace("Best target selector %s->%s score = %d", attacker->getDescription(), u->getDescription(), score);

					return score;
				});
		}

		auto defender = exchangeBattle.getForUpdate(targetUnit->unitId());
		auto shooting = cb->battleCanShoot(attacker.get());
		const int totalAttacks = attacker->getTotalAttacks(shooting);

		if(canUseAp && activeUnit == ap.attack.attacker && targetUnit == ap.attack.defender)
		{
			v.trackAttack(ap, &exchangeBattle);
		}
		else
		{
			for(int i = 0; i < totalAttacks; i++)
			{
				v.trackAttack(attacker, defender, shooting, isOur, cb);

				if(!attacker->alive() || !defender->alive())
					break;
			}
		}

		canUseAp = false;

		vstd::erase_if(attackerQueue, [&](const battle::Unit * u) -> bool
			{
				return !exchangeBattle.getForUpdate(u->unitId())->alive();
			});

		vstd::erase_if(oppositeQueue, [&](const battle::Unit * u) -> bool
			{
				return !exchangeBattle.getForUpdate(u->unitId())->alive();
			});
	}

	v.adjustPositions(melleeAttackers, ap, reachabilityMap);

	logAi->trace("Exchange score: %ld", v.getScore());

	return v.getScore();
}

void BattleExchangeVariant::adjustPositions(
	std::vector<const battle::Unit*> attackers,
	const AttackPossibility & ap,
	std::map<BattleHex, battle::Units> & reachabilityMap)
{
	auto hexes = ap.attack.defender->getSurroundingHexes();

	boost::sort(attackers, [&](const battle::Unit * u1, const battle::Unit * u2) -> bool
		{
			if(attackerValue[u1->unitId()].isRetalitated && !attackerValue[u2->unitId()].isRetalitated)
				return true;

			if(attackerValue[u2->unitId()].isRetalitated && !attackerValue[u1->unitId()].isRetalitated)
				return false;

			return attackerValue[u1->unitId()].value > attackerValue[u2->unitId()].value;
		});

	if(!ap.attack.shooting)
	{
		vstd::erase_if_present(hexes, ap.from);
		vstd::erase_if_present(hexes, ap.attack.attacker->occupiedHex(ap.attack.attackerPos));
	}

	int64_t notRealizedDps = 0;

	for(auto unit : attackers)
	{
		if(unit->unitId() == ap.attack.attacker->unitId())
			continue;

		if(!vstd::contains_if(hexes, [&](BattleHex h) -> bool
			{
				return vstd::contains(reachabilityMap[h], unit);
			}))
		{
			notRealizedDps += attackerValue[unit->unitId()].value;
			continue;
		}

		auto desiredPosition = vstd::minElementByFun(hexes, [&](BattleHex h) -> int64_t
			{
				auto score = vstd::contains(reachabilityMap[h], unit)
					? reachabilityMap[h].size()
					: 1000;

				if(unit->doubleWide())
				{
					auto backHex = unit->occupiedHex(h);

					if(vstd::contains(hexes, backHex))
						score += reachabilityMap[backHex].size();
				}

				return score;
			});

		hexes.erase(desiredPosition);
	}

	if(notRealizedDps > ap.attackValue() && notRealizedDps > attackerValue[ap.attack.attacker->unitId()].value)
	{
		dpsScore = EvaluationResult::INEFFECTIVE_SCORE;
	}
}

void BattleExchangeEvaluator::updateReachabilityMap(HypotheticBattle & hb)
{
	turnOrder.clear();

	hb.battleGetTurnOrder(turnOrder, 1000, 2);
	reachabilityMap.clear();

	for(int turn = 0; turn < turnOrder.size(); turn++)
	{
		auto & turnQueue = turnOrder[turn];
		HypotheticBattle turnBattle(env.get(), cb);

		for(const battle::Unit * unit : turnQueue)
		{
			auto unitReachability = turnBattle.getReachability(unit);

			for(BattleHex hex = BattleHex::TOP_LEFT; hex.isValid(); hex = hex + 1)
			{
				bool reachable = unitReachability.distances[hex] <= unit->Speed(turn);

				if(!reachable && unitReachability.accessibility[hex] == EAccessibility::ALIVE_STACK)
				{
					const battle::Unit * hexStack = cb->battleGetUnitByPos(hex);

					if(hexStack && cb->battleMatchOwner(unit, hexStack, false))
					{
						for(BattleHex neighbor : hex.neighbouringTiles())
						{
							reachable = unitReachability.distances[neighbor] <= unit->Speed(turn);

							if(reachable) break;
						}
					}
				}

				if(reachable)
				{
					reachabilityMap[hex].push_back(unit);
				}
			}
		}
	}
}

bool BattleExchangeEvaluator::checkPositionBlocksOurStacks(HypotheticBattle & hb, const battle::Unit * activeUnit, BattleHex position)
{
	int blockingScore = 0;

	for(int turn = 0; turn < turnOrder.size(); turn++)
	{
		auto & turnQueue = turnOrder[turn];
		HypotheticBattle turnBattle(env.get(), cb);

		auto unitToUpdate = turnBattle.getForUpdate(activeUnit->unitId());
		unitToUpdate->setPosition(position);

		for(const battle::Unit * unit : turnQueue)
		{
			if(unit->unitId() == unitToUpdate->unitId() || cb->battleMatchOwner(unit, activeUnit, false))
				continue;

			auto unitReachability = turnBattle.getReachability(unit);

			for(BattleHex hex = BattleHex::TOP_LEFT; hex.isValid(); hex = hex + 1)
			{
				bool enemyUnit = false;
				bool reachable = unitReachability.distances[hex] <= unit->Speed(turn);

				if(!reachable && unitReachability.accessibility[hex] == EAccessibility::ALIVE_STACK)
				{
					const battle::Unit * hexStack = turnBattle.battleGetUnitByPos(hex);

					if(hexStack && cb->battleMatchOwner(unit, hexStack, false))
					{
						enemyUnit = true;

						for(BattleHex neighbor : hex.neighbouringTiles())
						{
							reachable = unitReachability.distances[neighbor] <= unit->Speed(turn);

							if(reachable) break;
						}
					}
				}

				if(!reachable && vstd::contains(reachabilityMap[hex], unit))
				{
					blockingScore += enemyUnit ? 100 : 1;
				}
			}
		}
	}

	logAi->trace("Position %d, blocking score %d", position.hex, blockingScore);

	return blockingScore > 50;
}
