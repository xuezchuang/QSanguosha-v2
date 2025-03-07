﻿#include "room.h"
#include "engine.h"
#include "settings.h"
#include "standard.h"
#include "ai.h"
#include "scenario.h"
#include "gamerule.h"
#include "banpair.h"
#include "roomthread3v3.h"
#include "roomthreadxmode.h"
#include "roomthread1v1.h"
#include "server.h"
#include "generalselector.h"
#include "json.h"
#include "structs.h"
#include "miniscenarios.h"
#include "lua.hpp"
#include "exppattern.h"
#include "util.h"
#include "wrapped-card.h"
#include "roomthread.h"
#include "clientstruct.h"

#ifdef QSAN_UI_LIBRARY_AVAILABLE
#pragma message WARN("UI elements detected in server side!!!")
#endif

using namespace QSanProtocol;

Room::Room(QObject *parent, const QString &mode)
    : QThread(parent), mode(mode), current(NULL), pile1(Sanguosha->getRandomCards()),
    m_drawPile(&pile1), m_discardPile(&pile2),
    game_started(false), game_finished(false), game_paused(false), L(NULL), thread(NULL),
    thread_3v3(NULL), thread_xmode(NULL), thread_1v1(NULL), _m_semRaceRequest(0), _m_semRoomMutex(1),
    _m_raceStarted(false), provided(NULL), has_provided(false),
    m_surrenderRequestReceived(false), _virtual(false), _m_roomState(false)
{
    static int s_global_room_id = 0;
    _m_Id = s_global_room_id++;
    _m_lastMovementId = 0;
    player_count = Sanguosha->getPlayerCount(mode);
    scenario = Sanguosha->getScenario(mode);

    initCallbacks();

    L = CreateLuaState();
    if (!DoLuaScript(L, "lua/sanguosha.lua") || !DoLuaScript(L, "lua/ai/smart-ai.lua"))
        L = NULL;
    connect(this,SIGNAL(signalSetProperty(ServerPlayer*,const char*,QVariant)),this,
            SLOT(slotSetProperty(ServerPlayer*,const char*,QVariant)),Qt::QueuedConnection);
}

Room::~Room()
{
    lua_close(L);
	if (thread != NULL)
		delete thread;
}

void Room::initCallbacks()
{
    // init request response pair
    m_requestResponsePair[S_COMMAND_PLAY_CARD] = S_COMMAND_RESPONSE_CARD;
    m_requestResponsePair[S_COMMAND_NULLIFICATION] = S_COMMAND_RESPONSE_CARD;
    m_requestResponsePair[S_COMMAND_SHOW_CARD] = S_COMMAND_RESPONSE_CARD;
    m_requestResponsePair[S_COMMAND_ASK_PEACH] = S_COMMAND_RESPONSE_CARD;
    m_requestResponsePair[S_COMMAND_PINDIAN] = S_COMMAND_RESPONSE_CARD;
    m_requestResponsePair[S_COMMAND_EXCHANGE_CARD] = S_COMMAND_DISCARD_CARD;
    m_requestResponsePair[S_COMMAND_CHOOSE_DIRECTION] = S_COMMAND_MULTIPLE_CHOICE;
    m_requestResponsePair[S_COMMAND_LUCK_CARD] = S_COMMAND_INVOKE_SKILL;

    // client request handlers
    m_callbacks[S_COMMAND_SURRENDER] = &Room::processRequestSurrender;
    m_callbacks[S_COMMAND_CHEAT] = &Room::processRequestCheat;

    // Client notifications
    m_callbacks[S_COMMAND_TOGGLE_READY] = &Room::toggleReadyCommand;
    m_callbacks[S_COMMAND_ADD_ROBOT] = &Room::addRobotCommand;

    m_callbacks[S_COMMAND_SPEAK] = &Room::speakCommand;
    m_callbacks[S_COMMAND_TRUST] = &Room::trustCommand;
    m_callbacks[S_COMMAND_PAUSE] = &Room::pauseCommand;

    //Client request
    m_callbacks[S_COMMAND_NETWORK_DELAY_TEST] = &Room::networkDelayTestCommand;
}

ServerPlayer *Room::getCurrent() const
{
    return current;
}

void Room::setCurrent(ServerPlayer *current)
{
    this->current = current;
}

int Room::alivePlayerCount() const
{
    return m_alivePlayers.count();
}

bool Room::notifyUpdateCard(ServerPlayer *player, int cardId, const Card *newCard)
{
    JsonArray val;
    Q_ASSERT(newCard);
    QString className = newCard->getClassName();
    val << cardId << newCard->getSuit() << newCard->getNumber() << className << newCard->getSkillName() << newCard->objectName() << JsonUtils::toJsonArray(newCard->getFlags());
    doNotify(player, S_COMMAND_UPDATE_CARD, val);
    return true;
}

bool Room::broadcastUpdateCard(const QList<ServerPlayer *> &players, int cardId, const Card *newCard)
{
    foreach(ServerPlayer *player, players)
        notifyUpdateCard(player, cardId, newCard);
    return true;
}

bool Room::notifyResetCard(ServerPlayer *player, int cardId)
{
    doNotify(player, S_COMMAND_UPDATE_CARD, cardId);
    return true;
}

bool Room::broadcastResetCard(const QList<ServerPlayer *> &players, int cardId)
{
    foreach(ServerPlayer *player, players)
        notifyResetCard(player, cardId);
    return true;
}

QList<ServerPlayer *> Room::getPlayers() const
{
    return m_players;
}

QList<ServerPlayer *> Room::getAllPlayers(bool include_dead) const
{
    QList<ServerPlayer *> count_players = m_players;
    if (current == NULL)
        return count_players;

    ServerPlayer *starter = current;
    int index = count_players.indexOf(starter);
    if (index == -1)
        return count_players;

    QList<ServerPlayer *> all_players;
    for (int i = index; i < count_players.length(); i++) {
        if (include_dead || count_players[i]->isAlive())
            all_players << count_players[i];
    }

    for (int i = 0; i < index; i++) {
        if (include_dead || count_players[i]->isAlive())
            all_players << count_players[i];
    }

    if (current->getPhase() == Player::NotActive && all_players.contains(current)) {
        all_players.removeOne(current);
        all_players.append(current);
    }

    return all_players;
}

QList<ServerPlayer *> Room::getOtherPlayers(ServerPlayer *except, bool include_dead) const
{
    QList<ServerPlayer *> other_players = getAllPlayers(include_dead);
    if (except && (except->isAlive() || include_dead))
        other_players.removeOne(except);
    return other_players;
}

QList<ServerPlayer *> Room::getAlivePlayers() const
{
    return m_alivePlayers;
}

void Room::output(const QString &message)
{
    emit room_message(message);
}

void Room::outputEventStack()
{
    QString msg = "End of Event Stack.";
    foreach(EventTriplet triplet, *thread->getEventStack())
        msg.prepend(triplet.toString());
    msg.prepend("Event Stack:\n");
    output(msg);
}

void Room::enterDying(ServerPlayer *player, DamageStruct *reason)
{
    setPlayerFlag(player, "Global_Dying");
    QStringList currentdying = getTag("CurrentDying").toStringList();
    currentdying << player->objectName();
    setTag("CurrentDying", QVariant::fromValue(currentdying));

    JsonArray arg;
    arg << QSanProtocol::S_GAME_EVENT_PLAYER_DYING << player->objectName();
    doBroadcastNotify(QSanProtocol::S_COMMAND_LOG_EVENT, arg);

    DyingStruct dying;
    dying.who = player;
    dying.damage = reason;
    QVariant dying_data = QVariant::fromValue(dying);

    bool enterdying = thread->trigger(EnterDying, this, player, dying_data);

    if (!(player->isDead() || player->getHp() > 0 || enterdying)) {
        foreach(ServerPlayer *p, getAllPlayers()) {
            if (thread->trigger(Dying, this, p, dying_data) || player->getHp() > 0 || player->isDead())
                break;
        }

        if (player->isAlive()) {
            if (player->getHp() > 0) {
                setPlayerFlag(player, "-Global_Dying");
            } else {
                LogMessage log;
                log.type = "#AskForPeaches";
                log.from = player;
                log.to = getAllPlayers();
                log.arg = QString::number(1 - player->getHp());
                sendLog(log);

                foreach(ServerPlayer *saver, getAllPlayers()) {
                    if (player->getHp() > 0 || player->isDead())
                        break;

                    QString cd = saver->property("currentdying").toString();
                    setPlayerProperty(saver, "currentdying", player->objectName());
                    thread->trigger(AskForPeaches, this, saver, dying_data);
                    setPlayerProperty(saver, "currentdying", cd);
                }
                thread->trigger(AskForPeachesDone, this, player, dying_data);

                setPlayerFlag(player, "-Global_Dying");
            }
        }
    } else {
        setPlayerFlag(player, "-Global_Dying");
    }

    currentdying = getTag("CurrentDying").toStringList();
    currentdying.removeOne(player->objectName());
    setTag("CurrentDying", QVariant::fromValue(currentdying));

    if (player->isAlive()) {
        JsonArray arg;
        arg << QSanProtocol::S_GAME_EVENT_PLAYER_QUITDYING << player->objectName();
        doBroadcastNotify(QSanProtocol::S_COMMAND_LOG_EVENT, arg);
    }
    thread->trigger(QuitDying, this, player, dying_data);
}

ServerPlayer *Room::getCurrentDyingPlayer() const
{
    QStringList currentdying = getTag("CurrentDying").toStringList();
    if (currentdying.isEmpty()) return NULL;
    QString dyingobj = currentdying.last();
    ServerPlayer *who = NULL;
    foreach (ServerPlayer *p, m_alivePlayers) {
        if (p->objectName() == dyingobj) {
            who = p;
            break;
        }
    }
    return who;
}

void Room::revivePlayer(ServerPlayer *player, bool sendlog)
{
    int turn = player->getMark("Global_TurnCount");
    player->setAlive(true);
    player->throwAllMarks(false);
    broadcastProperty(player, "alive");
    setEmotion(player, "revive");
    setPlayerMark(player, "Global_TurnCount", turn);

    m_alivePlayers.clear();
    foreach (ServerPlayer *player, m_players) {
        if (player->isAlive())
            m_alivePlayers << player;
    }

    for (int i = 0; i < m_alivePlayers.length(); i++) {
        m_alivePlayers.at(i)->setSeat(i + 1);
        broadcastProperty(m_alivePlayers.at(i), "seat");
    }

    doBroadcastNotify(S_COMMAND_REVIVE_PLAYER, QVariant(player->objectName()));
    updateStateItem();

    if (sendlog) {
        LogMessage log;
        log.type = "#Revive";
        log.from = player;
        sendLog(log);
    }
}

static bool CompareByRole(ServerPlayer *player1, ServerPlayer *player2)
{
    int role1 = player1->getRoleEnum();
    int role2 = player2->getRoleEnum();

    if (role1 != role2)
        return role1 < role2;
    else
        return player1->isAlive();
}

void Room::updateStateItem()
{
    QList<ServerPlayer *> players = this->m_players;
    qSort(players.begin(), players.end(), CompareByRole);
    QString roles;
    foreach (ServerPlayer *p, players) {
        QChar c = "ZCFN"[p->getRoleEnum()];
        if (p->isDead())
            c = c.toLower();

        roles.append(c);
    }

    doBroadcastNotify(S_COMMAND_UPDATE_STATE_ITEM, QVariant(roles));
}

void Room::killPlayer(ServerPlayer *victim, DamageStruct *reason)
{
    ServerPlayer *killer = reason ? reason->from : NULL;
    QList<ServerPlayer *> players_with_victim = getAllPlayers();

    victim->setAlive(false);

    int index = m_alivePlayers.indexOf(victim);
    for (int i = index + 1; i < m_alivePlayers.length(); i++) {
        ServerPlayer *p = m_alivePlayers.at(i);
        p->setSeat(p->getSeat() - 1);
        broadcastProperty(p, "seat");
    }

    m_alivePlayers.removeOne(victim);

    DeathStruct death;
    death.who = victim;
    death.damage = reason;
    QVariant data = QVariant::fromValue(death);
    thread->trigger(BeforeGameOverJudge, this, victim, data);

    updateStateItem();

    LogMessage log;
    log.type = killer ? (killer == victim ? "#Suicide" : "#Murder") : "#Contingency";
    log.to << victim;
    log.arg = Config.EnableHegemony ? victim->getKingdom() : victim->getRole();
    log.from = killer;
    sendLog(log);

    broadcastProperty(victim, "alive");
    broadcastProperty(victim, "role");

    doBroadcastNotify(S_COMMAND_KILL_PLAYER, QVariant(victim->objectName()));

    thread->trigger(GameOverJudge, this, victim, data);

    foreach(ServerPlayer *p, players_with_victim)
        if (p->isAlive() || p == victim)
            thread->trigger(Death, this, p, data);

    victim->detachAllSkills();
    thread->trigger(BuryVictim, this, victim, data);

    if (!victim->isAlive() && Config.EnableAI) {
        bool expose_roles = true;
        foreach (ServerPlayer *player, m_alivePlayers) {
            if (!player->isOffline()) {
                expose_roles = false;
                break;
            }
        }

        if (expose_roles) {
            foreach (ServerPlayer *player, m_alivePlayers) {
                if (Config.EnableHegemony) {
                    QString role = player->getKingdom();
                    if (role == "god")
                        role = Sanguosha->getGeneral(player->property("basara_generals").toString().split("+").at(0))->getKingdom();
                    role = BasaraMode::getMappedRole(role);
                    broadcastProperty(player, "role", role);
                } else
                    broadcastProperty(player, "role");
            }

            static QStringList continue_list;
            if (continue_list.isEmpty())
                continue_list << "02_1v1" << "04_1v3" << "06_XMode";
            if (continue_list.contains(Config.GameMode))
                return;

            if (Config.AlterAIDelayAD)
                Config.AIDelay = Config.AIDelayAD;
            if (victim->isOnline() && Config.SurrenderAtDeath && mode != "02_1v1" && mode != "06_XMode"
                && askForSkillInvoke(victim, "surrender", "yes"))
                makeSurrender(victim);
        }
    }
}

void Room::judge(JudgeStruct &judge_struct)
{
    Q_ASSERT(judge_struct.who != NULL);

    JudgeStruct *judge_star = &judge_struct;
    QVariant data = QVariant::fromValue(judge_star);
    thread->trigger(StartJudge, this, judge_star->who, data);

    QList<ServerPlayer *> players = getAllPlayers();
    foreach (ServerPlayer *player, players) {
        if (thread->trigger(AskForRetrial, this, player, data))
            break;
    }

    thread->trigger(FinishRetrial, this, judge_star->who, data);
    thread->trigger(FinishJudge, this, judge_star->who, data);
}

void Room::sendJudgeResult(const JudgeStruct *judge)
{
    JsonArray arg;
    arg << QSanProtocol::S_GAME_EVENT_JUDGE_RESULT << judge->card->getEffectiveId() << judge->isEffected() << judge->who->objectName() << judge->reason;
    doBroadcastNotify(QSanProtocol::S_COMMAND_LOG_EVENT, arg);
}

QList<int> Room::getNCards(int n, bool update_pile_number)
{
    QList<int> card_ids;
    for (int i = 0; i < n; i++)
        card_ids << drawCard();

    if (update_pile_number)
        doBroadcastNotify(S_COMMAND_UPDATE_PILE, QVariant(m_drawPile->length()));

    return card_ids;
}

QStringList Room::aliveRoles(ServerPlayer *except) const
{
    QStringList roles;
    foreach (ServerPlayer *player, m_alivePlayers) {
        if (player != except)
            roles << player->getRole();
    }

    return roles;
}

void Room::gameOver(const QString &winner)
{
    QStringList all_roles;
    foreach (ServerPlayer *player, m_players) {
        all_roles << player->getRole();
        if (player->getHandcardNum() > 0) {
            QStringList handcards;
            foreach(const Card *card, player->getHandcards())
                handcards << Sanguosha->getEngineCard(card->getId())->getLogName();
            QString handcard = handcards.join(", ").toUtf8().toBase64();
            setPlayerProperty(player, "last_handcards", handcard);
        }
    }

    game_finished = true;

    emit game_over(winner);

    if (mode.contains("_mini_")) {
        ServerPlayer *playerWinner = NULL;
        QStringList winners = winner.split("+");
        foreach (ServerPlayer *sp, m_players) {
            if (sp->getState() != "robot"
                && (winners.contains(sp->getRole())
                || winners.contains(sp->objectName()))) {
                playerWinner = sp;
                break;
            }
        }

        if (playerWinner) {
            QString id = Config.GameMode;
            id.replace("_mini_", "");
            int stage = Config.value("MiniSceneStage", 1).toInt();
            int current = id.toInt();
            if (current < Sanguosha->getMiniSceneCounts()) {
                if (current + 1 > stage) Config.setValue("MiniSceneStage", current + 1);
                QString mode = QString(MiniScene::S_KEY_MINISCENE).arg(QString::number(current + 1));
                Config.setValue("GameMode", mode);
                Config.GameMode = mode;
            }
        }
    }
    Config.AIDelay = Config.OriginAIDelay;

    if (!getTag("NextGameMode").toString().isNull()) {
        QString name = getTag("NextGameMode").toString();
        Config.GameMode = name;
        Config.setValue("GameMode", name);
        removeTag("NextGameMode");
    }
    if (!getTag("NextGameSecondGeneral").isNull()) {
        bool enable = getTag("NextGameSecondGeneral").toBool();
        Config.Enable2ndGeneral = enable;
        Config.setValue("Enable2ndGeneral", enable);
        removeTag("NextGameSecondGeneral");
    }

    JsonArray arg;
    arg << winner << JsonUtils::toJsonArray(all_roles);
    doBroadcastNotify(S_COMMAND_GAME_OVER, arg);
    throw GameFinished;
}

void Room::slashEffect(const SlashEffectStruct &effect)
{
    QVariant data = QVariant::fromValue(effect);
    if (thread->trigger(SlashEffected, this, effect.to, data)) {
        if (!effect.to->hasFlag("Global_NonSkillNullify"))
            setEmotion(effect.to, "skill_nullify");
        else
            effect.to->setFlags("-Global_NonSkillNullify");
        if (effect.slash)
            effect.to->removeQinggangTag(effect.slash);
    }
}

void Room::slashResult(const SlashEffectStruct &effect, const Card *jink)
{
    SlashEffectStruct result_effect = effect;
    result_effect.jink = jink;
    QVariant data = QVariant::fromValue(result_effect);

    if (jink == NULL) {
        if (effect.to->isAlive())
            thread->trigger(SlashHit, this, effect.from, data);
    } else {
        if (effect.to->isAlive()) {
            if (jink->getSkillName() != "eight_diagram")
                setEmotion(effect.to, "jink");
        }
        if (effect.slash)
            effect.to->removeQinggangTag(effect.slash);
        thread->trigger(SlashMissed, this, effect.from, data);
    }
}

void Room::attachSkillToPlayer(ServerPlayer *player, const QString &skill_name)
{
    player->acquireSkill(skill_name);
    doNotify(player, S_COMMAND_ATTACH_SKILL, QVariant(skill_name));
}

void Room::detachSkillFromPlayer(ServerPlayer *player, const QString &skill_name, bool is_equip, bool acquire_only)
{
    if (!player->hasSkill(skill_name, true)) return;

    if (player->getAcquiredSkills().contains(skill_name))
        player->detachSkill(skill_name);
    else if (!acquire_only)
        player->loseSkill(skill_name);
    else
        return;

    const Skill *skill = Sanguosha->getSkill(skill_name);
    if (skill && skill->isVisible()) {
        JsonArray args;
        args << QSanProtocol::S_GAME_EVENT_DETACH_SKILL << player->objectName() << skill_name;
        doBroadcastNotify(QSanProtocol::S_COMMAND_LOG_EVENT, args);

        if (!is_equip) {
            LogMessage log;
            log.type = "#LoseSkill";
            log.from = player;
            log.arg = skill_name;
            sendLog(log);

            QVariant data = skill_name;
            thread->trigger(EventLoseSkill, this, player, data);
        }

        foreach (const Skill *skill, Sanguosha->getRelatedSkills(skill_name)) {
            if (skill->isVisible())
                detachSkillFromPlayer(player, skill->objectName());
        }
    }
}

void Room::handleAcquireDetachSkills(ServerPlayer *player, const QStringList &skill_names, bool acquire_only)
{
    if (skill_names.isEmpty()) return;
    QList<bool> isLost;
    QStringList triggerList;
    foreach (QString skill_name, skill_names) {
        if (skill_name.startsWith("-")) {
            QString actual_skill = skill_name.mid(1);
            if (!player->hasSkill(actual_skill, true)) continue;
            if (player->getAcquiredSkills().contains(actual_skill))
                player->detachSkill(actual_skill);
            else if (!acquire_only)
                player->loseSkill(actual_skill);
            else
                continue;
            const Skill *skill = Sanguosha->getSkill(actual_skill);
            if (skill && skill->isVisible()) {
                JsonArray args;
                args << QSanProtocol::S_GAME_EVENT_DETACH_SKILL << player->objectName() << actual_skill;
                doBroadcastNotify(QSanProtocol::S_COMMAND_LOG_EVENT, args);

                LogMessage log;
                log.type = "#LoseSkill";
                log.from = player;
                log.arg = actual_skill;
                sendLog(log);

                triggerList << actual_skill;
                isLost << true;

                foreach (const Skill *skill, Sanguosha->getRelatedSkills(actual_skill)) {
                    if (!skill->isVisible())
                        detachSkillFromPlayer(player, skill->objectName());
                }
            }
        } else {
            const Skill *skill = Sanguosha->getSkill(skill_name);
            if (!skill) continue;
            bool acquired = false;
            if (player->getAcquiredSkills().contains(skill_name)) acquired = true;
            player->acquireSkill(skill_name);
            if (acquired) continue;

            if (skill->inherits("TriggerSkill")) {
                const TriggerSkill *trigger_skill = qobject_cast<const TriggerSkill *>(skill);
                thread->addTriggerSkill(trigger_skill);
            }
            if (skill->getFrequency() == Skill::Limited && !skill->getLimitMark().isEmpty())
                setPlayerMark(player, skill->getLimitMark(), 1);

            if (skill->isVisible()) {
                JsonArray args;
                args << QSanProtocol::S_GAME_EVENT_ACQUIRE_SKILL << player->objectName() << skill_name;
                doBroadcastNotify(QSanProtocol::S_COMMAND_LOG_EVENT, args);

                foreach (const Skill *related_skill, Sanguosha->getRelatedSkills(skill_name)) {
                    if (!related_skill->isVisible())
                        acquireSkill(player, related_skill);
                }

                triggerList << skill_name;
                isLost << false;
            }
        }
    }
    if (!triggerList.isEmpty()) {
        for (int i = 0; i < triggerList.length(); i++) {
            QVariant data = triggerList.at(i);
            thread->trigger(isLost.at(i) ? EventLoseSkill : EventAcquireSkill, this, player, data);
        }
    }
}

void Room::handleAcquireDetachSkills(ServerPlayer *player, const QString &skill_names, bool acquire_only)
{
    handleAcquireDetachSkills(player, skill_names.split("|"), acquire_only);
}

bool Room::doRequest(ServerPlayer *player, QSanProtocol::CommandType command, const QVariant &arg, bool wait)
{
    time_t timeOut = ServerInfo.getCommandTimeout(command, S_SERVER_INSTANCE);
    return doRequest(player, command, arg, timeOut, wait);
}

bool Room::doRequest(ServerPlayer *player, QSanProtocol::CommandType command, const QVariant &arg, time_t timeOut, bool wait)
{
    Packet packet(S_SRC_ROOM | S_TYPE_REQUEST | S_DEST_CLIENT, command);
    packet.setMessageBody(arg);
    player->acquireLock(ServerPlayer::SEMA_MUTEX);
    player->m_isClientResponseReady = false;
    player->drainLock(ServerPlayer::SEMA_COMMAND_INTERACTIVE);
    player->setClientReply(QVariant());
    player->setClientReplyString(QString());
    player->m_isWaitingReply = true;
    player->m_expectedReplySerial = packet.globalSerial;
    if (m_requestResponsePair.contains(command))
        player->m_expectedReplyCommand = m_requestResponsePair[command];
    else
        player->m_expectedReplyCommand = command;

    player->invoke(&packet);
    player->releaseLock(ServerPlayer::SEMA_MUTEX);

    if (wait)
        return getResult(player, timeOut);
    else
        return true;
}

bool Room::doBroadcastRequest(QList<ServerPlayer *> &players, QSanProtocol::CommandType command)
{
    time_t timeOut = ServerInfo.getCommandTimeout(command, S_SERVER_INSTANCE);
    return doBroadcastRequest(players, command, timeOut*10);
}

bool Room::doBroadcastRequest(QList<ServerPlayer *> &players, QSanProtocol::CommandType command, time_t timeOut)
{
    foreach(ServerPlayer *player, players)
        doRequest(player, command, player->m_commandArgs, timeOut, false);

    QTime timer;
    time_t remainTime = timeOut;
    timer.start();
    foreach (ServerPlayer *player, players) {
        remainTime = timeOut - timer.elapsed();
        if (remainTime < 0) remainTime = 0;
        getResult(player, remainTime);
    }
    return true;
}

ServerPlayer *Room::doBroadcastRaceRequest(QList<ServerPlayer *> &players, QSanProtocol::CommandType command,
    time_t timeOut, ResponseVerifyFunction validateFunc, void *funcArg)
{
    _m_semRoomMutex.acquire();
    _m_raceStarted = true;
    _m_raceWinner = NULL;
    while (_m_semRaceRequest.tryAcquire(1)) {
    } //drain lock
    _m_semRoomMutex.release();
    Countdown countdown;
    countdown.max = timeOut;
    countdown.type = Countdown::S_COUNTDOWN_USE_SPECIFIED;
    if (command == S_COMMAND_NULLIFICATION)
        notifyMoveFocus(getAllPlayers(), command, countdown);
    else
        notifyMoveFocus(players, command, countdown);
    foreach(ServerPlayer *player, players)
        doRequest(player, command, player->m_commandArgs, timeOut, false);

    ServerPlayer *winner = getRaceResult(players, command, timeOut, validateFunc, funcArg);
    return winner;
}

ServerPlayer *Room::getRaceResult(QList<ServerPlayer *> &players, QSanProtocol::CommandType, time_t timeOut,
    ResponseVerifyFunction validateFunc, void *funcArg)
{
    QTime timer;
    timer.start();
    bool validResult = false;
    for (int i = 0; i < players.size(); i++) {
        time_t timeRemain = timeOut - timer.elapsed();
        if (timeRemain < 0) timeRemain = 0;
        bool tryAcquireResult = true;
        if (Config.OperationNoLimit)
            _m_semRaceRequest.acquire();
        else
            tryAcquireResult = _m_semRaceRequest.tryAcquire(1, timeRemain);

        if (!tryAcquireResult)
            _m_semRoomMutex.tryAcquire(1);
        // So that processResponse cannot update raceWinner when we are reading it.

        if (_m_raceWinner == NULL) {
            _m_semRoomMutex.release();
            continue;
        }

        if (validateFunc == NULL
            || (_m_raceWinner->m_isClientResponseReady
            && (this->*validateFunc)(_m_raceWinner, _m_raceWinner->getClientReply(), funcArg))) {
            validResult = true;
            break;
        } else {
            // Don't give this player any more chance for this race
            _m_raceWinner->m_isWaitingReply = false;
            _m_raceWinner = NULL;
            _m_semRoomMutex.release();
        }
    }

    if (!validResult) _m_semRoomMutex.acquire();
    _m_raceStarted = false;
    foreach (ServerPlayer *player, players) {
        player->acquireLock(ServerPlayer::SEMA_MUTEX);
        player->m_expectedReplyCommand = S_COMMAND_UNKNOWN;
        player->m_isWaitingReply = false;
        player->m_expectedReplySerial = -1;
        player->releaseLock(ServerPlayer::SEMA_MUTEX);
    }
    _m_semRoomMutex.release();
    return _m_raceWinner;
}

bool Room::doNotify(ServerPlayer *player, QSanProtocol::CommandType command, const QVariant &arg)
{
    Packet packet(S_SRC_ROOM | S_TYPE_NOTIFICATION | S_DEST_CLIENT, command);
    packet.setMessageBody(arg);
    player->invoke(&packet);
    return true;
}

bool Room::doBroadcastNotify(const QList<ServerPlayer *> &players, QSanProtocol::CommandType command, const QVariant &arg)
{
    foreach(ServerPlayer *player, players)
        doNotify(player, command, arg);
    return true;
}

bool Room::doBroadcastNotify(QSanProtocol::CommandType command, const QVariant &arg)
{
    return doBroadcastNotify(m_players, command, arg);
}

// the following functions for Lua
bool Room::doNotify(ServerPlayer *player, int command, const char *arg)
{
    Packet packet(S_SRC_ROOM | S_TYPE_NOTIFICATION | S_DEST_CLIENT, (QSanProtocol::CommandType)command);
    JsonDocument doc = JsonDocument::fromJson(arg);
    if (doc.isValid()) {
        packet.setMessageBody(doc.toVariant());
        player->invoke(&packet);
    } else {
        output(QString("Fail to parse the Json Value %1").arg(arg));
    }
    return true;
}

bool Room::doBroadcastNotify(const QList<ServerPlayer *> &players, int command, const char *arg)
{
    foreach(ServerPlayer *player, players)
        doNotify(player, command, arg);
    return true;
}

bool Room::doBroadcastNotify(int command, const char *arg)
{
    return doBroadcastNotify(m_players, command, arg);
}

bool Room::doNotify(ServerPlayer *player, int command, const QVariant &arg)
{
    Packet packet(S_SRC_ROOM | S_TYPE_NOTIFICATION | S_DEST_CLIENT, (QSanProtocol::CommandType)command);
    packet.setMessageBody(arg);
    player->invoke(&packet);
    return true;
}

bool Room::doBroadcastNotify(const QList<ServerPlayer *> &players, int command, const QVariant &arg)
{
    foreach(ServerPlayer *player, players)
        doNotify(player, command, arg);
    return true;
}

bool Room::doBroadcastNotify(int command, const QVariant &arg)
{
    return doBroadcastNotify(m_players, command, arg);
}

// end for Lua

void Room::broadcastInvoke(const char *method, const QString &arg, ServerPlayer *except)
{
    broadcast(QString("%1 %2").arg(method).arg(arg), except);
}

void Room::broadcastInvoke(const QSanProtocol::AbstractPacket *packet, ServerPlayer *except)
{
    broadcast(packet->toString(), except);
}

bool Room::getResult(ServerPlayer *player, time_t timeOut)
{
    Q_ASSERT(player->m_isWaitingReply);
    bool validResult = false;
    player->acquireLock(ServerPlayer::SEMA_MUTEX);

    if (player->isOnline()) {
        player->releaseLock(ServerPlayer::SEMA_MUTEX);

        if (Config.OperationNoLimit)
            player->acquireLock(ServerPlayer::SEMA_COMMAND_INTERACTIVE);
        else
            player->tryAcquireLock(ServerPlayer::SEMA_COMMAND_INTERACTIVE, timeOut);

        // Note that we rely on processResponse to filter out all unrelevant packet.
        // By the time the lock is released, m_clientResponse must be the right message
        // assuming the client side is not tampered.

        // Also note that lock can be released when a player switch to trust or offline status.
        // It is ensured by trustCommand and reportDisconnection that the player reports these status
        // is the player waiting the lock. In these cases, the serial number and command type doesn't matter.
        player->acquireLock(ServerPlayer::SEMA_MUTEX);
        validResult = player->m_isClientResponseReady;
    }
    player->m_expectedReplyCommand = S_COMMAND_UNKNOWN;
    player->m_isWaitingReply = false;
    player->m_expectedReplySerial = -1;
    player->releaseLock(ServerPlayer::SEMA_MUTEX);
    return validResult;
}

bool Room::notifyMoveFocus(ServerPlayer *player)
{
    QList<ServerPlayer *> players;
    players.append(player);
    Countdown countdown;
    countdown.type = Countdown::S_COUNTDOWN_NO_LIMIT;
    return notifyMoveFocus(players, S_COMMAND_MOVE_FOCUS, countdown);
}

bool Room::notifyMoveFocus(ServerPlayer *player, CommandType command)
{
    QList<ServerPlayer *> players;
    players.append(player);
    Countdown countdown;
    countdown.max = ServerInfo.getCommandTimeout(command, S_CLIENT_INSTANCE);
    countdown.type = Countdown::S_COUNTDOWN_USE_SPECIFIED;
    return notifyMoveFocus(players, S_COMMAND_MOVE_FOCUS, countdown);
}

bool Room::notifyMoveFocus(const QList<ServerPlayer *> &players, CommandType command, Countdown countdown)
{
    JsonArray arg;
    JsonArray arg1;
    int n = players.length();
    for (int i = 0; i < n; ++i)
        arg1 << players.value(i)->objectName();
    arg << QVariant(arg1) << command << countdown.toVariant();
    return doBroadcastNotify(S_COMMAND_MOVE_FOCUS, arg);
}

bool Room::askForSkillInvoke(ServerPlayer *player, const QString &skill_name, const QVariant &data)
{
    tryPause();
    notifyMoveFocus(player, S_COMMAND_INVOKE_SKILL);

    bool invoked = false;
    AI *ai = player->getAI();
    if (ai) {
        invoked = ai->askForSkillInvoke(skill_name, data);
        const Skill *skill = Sanguosha->getSkill(skill_name);
        if (invoked && !(skill && (skill->getFrequency(player) == Skill::Frequent || skill->getFrequency(player) == Skill::Limited)))
            thread->delay();
    } else {
        JsonArray skillCommand;
        if (data.type() == QVariant::String)
            skillCommand  << skill_name << data.toString();
        else {
            ServerPlayer *p = data.value<ServerPlayer *>();
            QString data_str;
            if (p != NULL)
                data_str = "playerdata:" + p->objectName();
            skillCommand << skill_name << data_str;
        }

        if (!doRequest(player, S_COMMAND_INVOKE_SKILL, skillCommand, true)) {
            invoked = false;
        } else {
            QVariant clientReply = player->getClientReply();
            if (clientReply.canConvert(QVariant::Bool))
                invoked = clientReply.toBool();
        }
    }

    if (invoked) {
        JsonArray msg;
        msg << skill_name << player->objectName();
        doBroadcastNotify(S_COMMAND_INVOKE_SKILL, msg);
        notifySkillInvoked(player, skill_name);
    }

    QString decisionString = QString("skillInvoke:" + skill_name);
    ServerPlayer *p = data.value<ServerPlayer *>();
    if (p != NULL)
        decisionString = decisionString + ":" + p->objectName();
    decisionString = decisionString + ":" + (invoked ? "yes" : "no");
    QVariant decisionData = QVariant::fromValue(decisionString);
    thread->trigger(ChoiceMade, this, player, decisionData);
    return invoked;
}

QString Room::askForChoice(ServerPlayer *player, const QString &skill_name, const QString &choices, const QVariant &data)
{
    tryPause();
    notifyMoveFocus(player, S_COMMAND_MULTIPLE_CHOICE);

    QStringList validChoices = choices.split("+");
    Q_ASSERT(!validChoices.isEmpty());

    AI *ai = player->getAI();
    QString answer;
    if (validChoices.size() == 1)
        answer = validChoices.first();
    else {
        if (ai) {
            answer = ai->askForChoice(skill_name, choices, data);
            thread->delay();
        } else {
            bool success = doRequest(player, S_COMMAND_MULTIPLE_CHOICE, JsonArray() << skill_name << choices, true);
            QVariant clientReply = player->getClientReply();
            if (!success || !clientReply.canConvert(QVariant::String))
                answer = "cancel";
            else
                answer = clientReply.toString();
        }
    }

    if (!validChoices.contains(answer))
        answer = validChoices.at(qrand() % validChoices.length());

    QVariant decisionData = QVariant::fromValue("skillChoice:" + skill_name + ":" + answer);
    thread->trigger(ChoiceMade, this, player, decisionData);
    return answer;
}

void Room::obtainCard(ServerPlayer *target, const Card *card, const CardMoveReason &reason, bool unhide)
{
    if (card == NULL) return;
    moveCardTo(card, NULL, target, Player::PlaceHand, reason, unhide);
}

void Room::obtainCard(ServerPlayer *target, const Card *card, bool unhide)
{
    if (card == NULL) return;
    CardMoveReason reason(CardMoveReason::S_REASON_GOTBACK, target->objectName());
    obtainCard(target, card, reason, unhide);
}

void Room::obtainCard(ServerPlayer *target, int card_id, bool unhide)
{
    obtainCard(target, Sanguosha->getCard(card_id), unhide);
}

bool Room::isCanceled(const CardEffectStruct &effect)
{
    if (!effect.card->isCancelable(effect))
        return false;

    QVariant decisionData = QVariant::fromValue(effect.to);
    setTag("NullifyingTarget", decisionData);
    decisionData = QVariant::fromValue(effect.from);
    setTag("NullifyingSource", decisionData);
    decisionData = QVariant::fromValue(effect.card);
    setTag("NullifyingCard", decisionData);
    setTag("NullifyingTimes", 0);
    return askForNullification(effect.card, effect.from, effect.to, true);
}

bool Room::verifyNullificationResponse(ServerPlayer *player, const QVariant &response, void *)
{
    const Card *card = NULL;
    if (player != NULL && response.canConvert<JsonArray>()) {
        JsonArray responseArray = response.value<JsonArray>();
        if (JsonUtils::isString(responseArray[0]))
            card = Card::Parse(responseArray[0].toString());
    }
    return card != NULL;
}

bool Room::askForNullification(const Card *trick, ServerPlayer *from, ServerPlayer *to, bool positive)
{
    _NullificationAiHelper aiHelper;
    aiHelper.m_from = from;
    aiHelper.m_to = to;
    aiHelper.m_trick = trick;
    return _askForNullification(trick, from, to, positive, aiHelper);
}

bool Room::_askForNullification(const Card *trick, ServerPlayer *from, ServerPlayer *to,
    bool positive, _NullificationAiHelper aiHelper)
{
    tryPause();

    _m_roomState.setCurrentCardUseReason(CardUseStruct::CARD_USE_REASON_RESPONSE_USE);
    QString trick_name = trick->objectName();
    QList<ServerPlayer *> validHumanPlayers;
    QList<ServerPlayer *> validAiPlayers;

    JsonArray arg;
    arg << trick_name;
    arg << (from ? QVariant(from->objectName()) : QVariant());
    arg << (to ? QVariant(to->objectName()) : QVariant());

    CardEffectStruct trickEffect;
    trickEffect.card = trick;
    trickEffect.from = from;
    trickEffect.to = to;
    QVariant data = QVariant::fromValue(trickEffect);
    foreach (ServerPlayer *player, m_alivePlayers) {
        if (player->hasNullification()) {
            if (!thread->trigger(TrickCardCanceling, this, player, data)) {
                if (player->isOnline()) {
                    player->m_commandArgs = arg;
                    validHumanPlayers << player;
                } else
                    validAiPlayers << player;
            }
        }
    }

    ServerPlayer *repliedPlayer = NULL;
    time_t timeOut = ServerInfo.getCommandTimeout(S_COMMAND_NULLIFICATION, S_SERVER_INSTANCE);
    if (!validHumanPlayers.isEmpty()) {
        if (trick->isKindOf("AOE") || trick->isKindOf("GlobalEffect")) {
            foreach(ServerPlayer *p, validHumanPlayers)
                doNotify(p, S_COMMAND_NULLIFICATION_ASKED, QVariant(trick->objectName()));
        }
        repliedPlayer = doBroadcastRaceRequest(validHumanPlayers, S_COMMAND_NULLIFICATION,
            timeOut, &Room::verifyNullificationResponse);
    }
    const Card *card = NULL;
    if (repliedPlayer != NULL) {
        JsonArray clientReply = repliedPlayer->getClientReply().value<JsonArray>();
        if (clientReply.size() > 0 && JsonUtils::isString(clientReply[0]))
            card = Card::Parse(clientReply[0].toString());
    }
    if (card == NULL) {
        foreach (ServerPlayer *player, validAiPlayers) {
            AI *ai = player->getAI();
            if (ai == NULL) continue;
            card = ai->askForNullification(aiHelper.m_trick, aiHelper.m_from, aiHelper.m_to, positive);
            if (card && player->isCardLimited(card, Card::MethodUse))
                card = NULL;
            if (card != NULL) {
                thread->delay();
                repliedPlayer = player;
                break;
            }
        }
    }

    if (card == NULL) return false;

    card = card->validateInResponse(repliedPlayer);

    if (card == NULL)
        return _askForNullification(trick, from, to, positive, aiHelper);

    LogMessage log;
    log.type = "#NullificationDetails";
    log.from = from;
    log.to << to;
    log.arg = trick_name;
    sendLog(log);
    useCard(CardUseStruct(card, repliedPlayer, QList<ServerPlayer *>()));

    QVariant _card = card;
    if (thread->trigger(NullificationEffect, this, repliedPlayer, _card))
        return _askForNullification(trick, from, to, positive, aiHelper);

#define TOOBJECTNAME ((to ? to->objectName() : repliedPlayer->objectName()))
    doAnimate(S_ANIMATE_NULLIFICATION, repliedPlayer->objectName(), TOOBJECTNAME);

    thread->delay(500);

    QVariant decisionData = QVariant::fromValue("Nullification:" + QString(trick->getClassName())
        + ":" + TOOBJECTNAME + ":" + (positive ? "true" : "false"));
#undef TOOBJECTNAME
    thread->trigger(ChoiceMade, this, repliedPlayer, decisionData);
    setTag("NullifyingTimes", getTag("NullifyingTimes").toInt() + 1);

    bool result = true;
    CardEffectStruct effect;
    effect.card = card;
    effect.to = repliedPlayer;
    if (card->isCancelable(effect))
        result = !_askForNullification(card, repliedPlayer, to, !positive, aiHelper);
    if (card->isVirtualCard())
        delete card;
    return result;
}

int Room::askForCardChosen(ServerPlayer *player, ServerPlayer *who, const QString &flags, const QString &reason,
    bool handcard_visible, Card::HandlingMethod method, const QList<int> &disabled_ids)
{
    tryPause();
    notifyMoveFocus(player, S_COMMAND_CHOOSE_CARD);

    // process dongcha
    if (who->objectName() == tag.value("Dongchaee").toString()
        && player->objectName() == tag.value("Dongchaer").toString())
        handcard_visible = true;

    if (handcard_visible && !who->isKongcheng()) {
        QList<int> handcards = who->handCards();
        JsonArray arg;
        arg << who->objectName();
        arg << JsonUtils::toJsonArray(handcards);
        doNotify(player, S_COMMAND_SET_KNOWN_CARDS, arg);
    }
    int card_id = Card::S_UNKNOWN_CARD_ID;
    if (who != player && !handcard_visible
        && (flags == "h"
        || (flags == "he" && !who->hasEquip())
        || (flags == "hej" && !who->hasEquip() && who->getJudgingArea().isEmpty())))
        card_id = who->getRandomHandCardId();
    else {
        AI *ai = player->getAI();
        if (ai) {
            thread->delay();
            card_id = ai->askForCardChosen(who, flags, reason, method);
            if (card_id == -1) {
                QList<const Card *> cards = who->getCards(flags);
                if (method == Card::MethodDiscard) {
                    foreach (const Card *card, cards) {
                        if (!player->canDiscard(who, card->getEffectiveId()) || disabled_ids.contains(card->getEffectiveId()))
                            cards.removeOne(card);
                    }
                }
                Q_ASSERT(!cards.isEmpty());
                card_id = cards.at(qrand() % cards.length())->getId();
            }
        } else {
            JsonArray arg;
            arg << who->objectName();
            arg << flags;
            arg << reason;
            arg << handcard_visible;
            arg << (int)method;
            arg << JsonUtils::toJsonArray(disabled_ids);
            bool success = doRequest(player, S_COMMAND_CHOOSE_CARD, arg, true);
            //@todo: check if the card returned is valid
            const QVariant &clientReply = player->getClientReply();
            if (!success || !JsonUtils::isNumber(clientReply)) {
                // randomly choose a card
                QList<const Card *> cards = who->getCards(flags);
                if (method == Card::MethodDiscard) {
                    foreach (const Card *card, cards) {
                        if (!player->canDiscard(who, card->getEffectiveId()) || disabled_ids.contains(card->getEffectiveId()))
                            cards.removeOne(card);
                    }
                }
                Q_ASSERT(!cards.isEmpty());
                card_id = cards.at(qrand() % cards.length())->getId();
            } else
                card_id = clientReply.toInt();

            if (card_id == Card::S_UNKNOWN_CARD_ID)
                card_id = who->getRandomHandCardId();
        }
    }

    Q_ASSERT(card_id != Card::S_UNKNOWN_CARD_ID);

    QVariant decisionData = QVariant::fromValue(QString("cardChosen:%1:%2:%3:%4").arg(reason).arg(card_id)
        .arg(player->objectName()).arg(who->objectName()));
    thread->trigger(ChoiceMade, this, player, decisionData);
    return card_id;
}

const Card *Room::askForCard(ServerPlayer *player, const QString &pattern, const QString &prompt,
    const QVariant &data, const QString &skill_name)
{
    return askForCard(player, pattern, prompt, data, Card::MethodDiscard, NULL, false, skill_name, false);
}

const Card *Room::askForCard(ServerPlayer *player, const QString &pattern, const QString &prompt,
    const QVariant &data, Card::HandlingMethod method, ServerPlayer *to,
    bool isRetrial, const QString &skill_name, bool isProvision)
{
    Q_ASSERT(pattern != "slash" || method != Card::MethodUse); // use askForUseSlashTo instead
    tryPause();
    notifyMoveFocus(player, S_COMMAND_RESPONSE_CARD);

    _m_roomState.setCurrentCardUsePattern(pattern);

    const Card *card = NULL;

    QStringList asked;
    asked << pattern << prompt;
    QVariant asked_data = QVariant::fromValue(asked);
    if ((method == Card::MethodUse || method == Card::MethodResponse) && !isRetrial && !player->hasFlag("continuing"))
        thread->trigger(CardAsked, this, player, asked_data);

    CardUseStruct::CardUseReason reason = CardUseStruct::CARD_USE_REASON_UNKNOWN;
    if (method == Card::MethodResponse)
        reason = CardUseStruct::CARD_USE_REASON_RESPONSE;
    else if (method == Card::MethodUse)
        reason = CardUseStruct::CARD_USE_REASON_RESPONSE_USE;
    _m_roomState.setCurrentCardUseReason(reason);

    if (player->hasFlag("continuing"))
        setPlayerFlag(player, "-continuing");
    if (has_provided || !player->isAlive()) {
        card = provided;
        if (player->isCardLimited(card, method)) card = NULL;
        provided = NULL;
        has_provided = false;
    } else {
        AI *ai = player->getAI();
        if (ai) {
            card = ai->askForCard(pattern, prompt, data);
            if (card && card->isKindOf("DummyCard") && card->subcardsLength() == 1)
                card = Sanguosha->getCard(card->getEffectiveId());
            if (card && player->isCardLimited(card, method)) card = NULL;
            if (card) thread->delay();
        } else {
            JsonArray arg;
            arg << pattern;
            arg << prompt;
            arg << int(method);
            bool success = doRequest(player, S_COMMAND_RESPONSE_CARD, arg, true);
            JsonArray clientReply = player->getClientReply().value<JsonArray>();
            if (success && !clientReply.isEmpty())
                card = Card::Parse(clientReply[0].toString());
        }
    }

    if (card == NULL) {
        QVariant decisionData = QVariant::fromValue(QString("cardResponded:%1:%2:_nil_").arg(pattern).arg(prompt));
        thread->trigger(ChoiceMade, this, player, decisionData);
        return NULL;
    }

    card = card->validateInResponse(player);
    const Card *result = NULL;

    if (card) {
        if (!card->isVirtualCard()) {
            WrappedCard *wrapped = Sanguosha->getWrappedCard(card->getEffectiveId());
            if (wrapped->isModified())
                broadcastUpdateCard(getPlayers(), card->getEffectiveId(), wrapped);
            else
                broadcastResetCard(getPlayers(), card->getEffectiveId());
        }

        if ((method == Card::MethodUse || method == Card::MethodResponse) && !isRetrial) {
            LogMessage log;
            log.card_str = card->toString();
            log.from = player;
            log.type = "#UseCard";
            if (method == Card::MethodResponse)
                log.type += "_Resp";
            sendLog(log);
            player->broadcastSkillInvoke(card);
        } else if (method == Card::MethodDiscard) {
            LogMessage log;
            log.type = skill_name.isEmpty() ? "$DiscardCard" : "$DiscardCardWithSkill";
            log.from = player;
            QList<int> to_discard;
            if (card->isVirtualCard())
                to_discard.append(card->getSubcards());
            else
                to_discard << card->getEffectiveId();
            log.card_str = IntList2StringList(to_discard).join("+");
            if (!skill_name.isEmpty())
                log.arg = skill_name;
            sendLog(log);
            if (!skill_name.isEmpty())
                notifySkillInvoked(player, skill_name);
        }
    }

    bool isHandcard = true;
    if (card) {
        QList<int> ids;
        if (!card->isVirtualCard()) ids << card->getEffectiveId();
        else ids = card->getSubcards();
        if (!ids.isEmpty()) {
            foreach (int id, ids) {
                if (getCardOwner(id) != player || getCardPlace(id) != Player::PlaceHand) {
                    isHandcard = false;
                    break;
                }
            }
        } else {
            isHandcard = false;
        }

        QVariant decisionData = QVariant::fromValue(QString("cardResponded:%1:%2:_%3_").arg(pattern).arg(prompt).arg(card->toString()));
        thread->trigger(ChoiceMade, this, player, decisionData);

        if (method == Card::MethodDiscard) {
            CardMoveReason reason(CardMoveReason::S_REASON_THROW, player->objectName());
            QList<int> ids;
            if (card->isVirtualCard())
                ids = card->getSubcards();
            else
                ids << card->getId();
            QList<CardsMoveStruct> moves;
            foreach (int id, ids) {
                CardsMoveStruct move(id, NULL, Player::DiscardPile, reason);
                moves << move;
            }
            moveCardsAtomic(moves, true);
        }

        if ((method == Card::MethodUse || method == Card::MethodResponse) && !isRetrial) {
            if (!card->getSkillName().isNull() && card->getSkillName(true) == card->getSkillName(false)
                && player->hasSkill(card->getSkillName()))
                notifySkillInvoked(player, card->getSkillName());
            CardResponseStruct resp(card, to, method == Card::MethodUse);
            resp.m_isHandcard = isHandcard;
            QVariant data = QVariant::fromValue(resp);
            thread->trigger(PreCardResponded, this, player, data);
            resp = data.value<CardResponseStruct>();

            if (method == Card::MethodUse) {
                CardMoveReason reason(CardMoveReason::S_REASON_LETUSE, player->objectName(), QString(), card->getSkillName(), QString());
                reason.m_extraData = QVariant::fromValue(card);
                QList<int> ids;
                if (card->isVirtualCard())
                    ids = card->getSubcards();
                else
                    ids << card->getId();
                QList<CardsMoveStruct> moves;
                foreach (int id, ids) {
                    CardsMoveStruct move(id, NULL, Player::PlaceTable, reason);
                    moves << move;
                }
                moveCardsAtomic(moves, true);
            } else {
                CardMoveReason reason(CardMoveReason::S_REASON_RESPONSE, player->objectName());
                reason.m_skillName = card->getSkillName();
                reason.m_extraData = QVariant::fromValue(card);
                QList<int> ids;
                if (card->isVirtualCard())
                    ids = card->getSubcards();
                else
                    ids << card->getId();
                QList<CardsMoveStruct> moves;
                foreach (int id, ids) {
                    CardsMoveStruct move(id, NULL, isProvision ? Player::PlaceTable : Player::DiscardPile, reason);
                    moves << move;
                }
                moveCardsAtomic(moves, false);
            }

            thread->trigger(CardResponded, this, player, data);
            if (method == Card::MethodUse) {
                QList<int> ids;
                if (card->isVirtualCard())
                    ids = card->getSubcards();
                else
                    ids << card->getId();
                QList<CardsMoveStruct> moves;
                CardMoveReason reason(CardMoveReason::S_REASON_LETUSE, player->objectName(),
                    QString(), card->getSkillName(), QString());
                reason.m_extraData = QVariant::fromValue(card);
                foreach (int id, ids) {
                    if (getCardPlace(id) == Player::PlaceTable) {
                        CardsMoveStruct move(id, player, NULL, Player::PlaceTable, Player::DiscardPile, reason);
                        moves << move;
                    }
                }
                moveCardsAtomic(moves, true);
                CardUseStruct card_use;
                card_use.card = card;
                card_use.from = player;
                if (to) card_use.to << to;
                QVariant data2 = QVariant::fromValue(card_use);
                thread->trigger(CardFinished, this, player, data2);
            }
        }
        result = card;
    } else {
        setPlayerFlag(player, "continuing");
        result = askForCard(player, pattern, prompt, data, method, to, isRetrial);
    }
    return result;
}

const Card *Room::askForUseCard(ServerPlayer *player, const QString &pattern, const QString &prompt, int notice_index,
    Card::HandlingMethod method, bool addHistory)
{
    Q_ASSERT(method != Card::MethodResponse);
    tryPause();
    notifyMoveFocus(player, S_COMMAND_RESPONSE_CARD);

    _m_roomState.setCurrentCardUsePattern(pattern);
    _m_roomState.setCurrentCardUseReason(CardUseStruct::CARD_USE_REASON_RESPONSE_USE);
    CardUseStruct card_use;

    bool isCardUsed = false;
    AI *ai = player->getAI();
    if (ai) {
        QString answer = ai->askForUseCard(pattern, prompt, method);
        if (answer != ".") {
            isCardUsed = true;
            card_use.from = player;
            card_use.parse(answer, this);
            thread->delay();
        }
    } else {
        JsonArray ask_str;
        ask_str << pattern;
        ask_str << prompt;
        ask_str << int(method);
        ask_str << notice_index;
        bool success = doRequest(player, S_COMMAND_RESPONSE_CARD, ask_str, true);
        if (success) {
            const QVariant &clientReply = player->getClientReply();
            isCardUsed = !clientReply.isNull();
            if (isCardUsed && card_use.tryParse(clientReply, this))
                card_use.from = player;
        }
    }
    if (isCardUsed && card_use.isValid(pattern)) {
        QVariant decisionData = QVariant::fromValue(card_use);
        thread->trigger(ChoiceMade, this, player, decisionData);
        if (!useCard(card_use, addHistory))
            return askForUseCard(player, pattern, prompt, notice_index, method, addHistory);
        return card_use.card;
    } else {
        QVariant decisionData = QVariant::fromValue("cardUsed:" + pattern + ":" + prompt + ":nil");
        thread->trigger(ChoiceMade, this, player, decisionData);
    }

    return NULL;
}

const Card *Room::askForUseSlashTo(ServerPlayer *slasher, QList<ServerPlayer *> victims, const QString &prompt,
    bool distance_limit, bool disable_extra, bool addHistory)
{
    Q_ASSERT(!victims.isEmpty());

    // The realization of this function in the Slash::onUse and Slash::targetFilter.
    setPlayerFlag(slasher, "slashTargetFix");
    if (!distance_limit)
        setPlayerFlag(slasher, "slashNoDistanceLimit");
    if (disable_extra)
        setPlayerFlag(slasher, "slashDisableExtraTarget");
    if (victims.length() == 1)
        setPlayerFlag(slasher, "slashTargetFixToOne");
    foreach(ServerPlayer *victim, victims)
        setPlayerFlag(victim, "SlashAssignee");

    const Card *slash = askForUseCard(slasher, "slash", prompt, -1, Card::MethodUse, addHistory);
    if (slash == NULL) {
        setPlayerFlag(slasher, "-slashTargetFix");
        setPlayerFlag(slasher, "-slashTargetFixToOne");
        foreach(ServerPlayer *victim, victims)
            setPlayerFlag(victim, "-SlashAssignee");
        if (slasher->hasFlag("slashNoDistanceLimit"))
            setPlayerFlag(slasher, "-slashNoDistanceLimit");
        if (slasher->hasFlag("slashDisableExtraTarget"))
            setPlayerFlag(slasher, "-slashDisableExtraTarget");
    }

    return slash;
}

const Card *Room::askForUseSlashTo(ServerPlayer *slasher, ServerPlayer *victim, const QString &prompt,
    bool distance_limit, bool disable_extra, bool addHistory)
{
    Q_ASSERT(victim != NULL);
    QList<ServerPlayer *> victims;
    victims << victim;
    return askForUseSlashTo(slasher, victims, prompt, distance_limit, disable_extra, addHistory);
}

int Room::askForAG(ServerPlayer *player, const QList<int> &card_ids, bool refusable, const QString &reason)
{
    tryPause();
    notifyMoveFocus(player, S_COMMAND_AMAZING_GRACE);
    Q_ASSERT(card_ids.length() > 0);

    int card_id = -1;
    if (card_ids.length() == 1 && !refusable)
        card_id = card_ids.first();
    else {
        AI *ai = player->getAI();
        if (ai) {
            thread->delay();
            card_id = ai->askForAG(card_ids, refusable, reason);
        } else {
            bool success = doRequest(player, S_COMMAND_AMAZING_GRACE, refusable, true);
            const QVariant &clientReply = player->getClientReply();
            if (success && JsonUtils::isNumber(clientReply))
                card_id = clientReply.toInt();
        }

        if (!card_ids.contains(card_id))
            card_id = refusable ? -1 : card_ids.first();
    }

    QVariant decisionData = QVariant::fromValue("AGChosen:" + reason + ":" + QString::number(card_id));
    thread->trigger(ChoiceMade, this, player, decisionData);

    return card_id;
}

const Card *Room::askForCardShow(ServerPlayer *player, ServerPlayer *requestor, const QString &reason)
{
    Q_ASSERT(!player->isKongcheng());
    tryPause();
    notifyMoveFocus(player, S_COMMAND_SHOW_CARD);
    const Card *card = NULL;

    AI *ai = player->getAI();
    if (ai)
        card = ai->askForCardShow(requestor, reason);
    else {
        if (player->getHandcardNum() == 1)
            card = player->getHandcards().first();
        else {
            bool success = doRequest(player, S_COMMAND_SHOW_CARD, requestor->getGeneralName(), true);
            JsonArray clientReply = player->getClientReply().value<JsonArray>();
            if (success && clientReply.size() > 0 && JsonUtils::isString(clientReply[0]))
                card = Card::Parse(clientReply[0].toString());
            if (card == NULL)
                card = player->getRandomHandCard();
        }
    }

    QVariant decisionData = QVariant::fromValue("cardShow:" + reason + ":_" + card->toString() + "_");
    thread->trigger(ChoiceMade, this, player, decisionData);
    return card;
}

const Card *Room::askForSinglePeach(ServerPlayer *player, ServerPlayer *dying)
{
    tryPause();
    notifyMoveFocus(player, S_COMMAND_ASK_PEACH);
    _m_roomState.setCurrentCardUseReason(CardUseStruct::CARD_USE_REASON_RESPONSE_USE);

    const Card *card = NULL;

    AI *ai = player->getAI();
    if (ai)
        card = ai->askForSinglePeach(dying);
    else {
        int peaches = 1 - dying->getHp();
        JsonArray arg;
        arg << dying->objectName();
        arg << peaches;
        bool success = doRequest(player, S_COMMAND_ASK_PEACH, arg, true);
        JsonArray clientReply = player->getClientReply().value<JsonArray>();
        if (!success || clientReply.isEmpty() || !JsonUtils::isString(clientReply[0]))
            return NULL;

        card = Card::Parse(clientReply[0].toString());
    }

    if (card && player->isCardLimited(card, Card::MethodUse))
        card = NULL;
    if (card != NULL)
        card = card->validateInResponse(player);
    else
        return NULL;

    const Card *result = NULL;
    if (card) {
        QVariant decisionData = QVariant::fromValue(QString("peach:%1:%2:%3")
            .arg(dying->objectName())
            .arg(1 - dying->getHp())
            .arg(card->toString()));
        thread->trigger(ChoiceMade, this, player, decisionData);
        result = card;
    } else
        result = askForSinglePeach(player, dying);
    return result;
}

void Room::addPlayerHistory(ServerPlayer *player, const QString &key, int times)
{
    if (player) {
        if (key == ".")
            player->clearHistory();
        else if (times == 0)
            player->clearHistory(key);
        else
            player->addHistory(key, times);
    }

    JsonArray arg;
    arg << key;
    arg << times;

    if (player)
        doNotify(player, S_COMMAND_ADD_HISTORY, arg);
    else
        doBroadcastNotify(S_COMMAND_ADD_HISTORY, arg);
}

void Room::setPlayerFlag(ServerPlayer *player, const QString &flag)
{
    if (flag.startsWith("-")) {
        QString set_flag = flag.mid(1);
        if (!player->hasFlag(set_flag)) return;
    }
    player->setFlags(flag);
    broadcastProperty(player, "flags", flag);
}

void Room::setPlayerProperty(ServerPlayer *player, const char *property_name, const QVariant &value)
{
#ifdef QT_DEBUG
    if(currentThread()!=player->thread())
    {
		playerPropertySet = false;
        emit signalSetProperty(player,property_name,value);
		while (!playerPropertySet) {}
    }
    else
    {
        player->setProperty(property_name, value);
    }
#else
	player->setProperty(property_name, value);
#endif // QT_DEBUG
	broadcastProperty(player, property_name);

    if (strcmp(property_name, "hp") == 0) {
        QVariant data = getTag("HpChangedData");
        thread->trigger(HpChanged, this, player, data);
    }

    if (strcmp(property_name, "maxhp") == 0)
        thread->trigger(MaxHpChanged, this, player);

    if (strcmp(property_name, "chained") == 0)
        thread->trigger(ChainStateChanged, this, player);
}

void Room::slotSetProperty(ServerPlayer *player, const char *property_name, const QVariant &value)
{
    player->setProperty(property_name, value);
	playerPropertySet = true;
}

void Room::setPlayerMark(ServerPlayer *player, const QString &mark, int value)
{
    player->setMark(mark, value);

    JsonArray arg;
    arg << player->objectName();
    arg << mark;
    arg << value;
    doBroadcastNotify(S_COMMAND_SET_MARK, arg);
}

void Room::addPlayerMark(ServerPlayer *player, const QString &mark, int add_num)
{
    int value = player->getMark(mark);
    value += add_num;
    setPlayerMark(player, mark, value);
}

void Room::removePlayerMark(ServerPlayer *player, const QString &mark, int remove_num)
{
    int value = player->getMark(mark);
    if (value == 0) return;
    value -= remove_num;
    value = qMax(0, value);
    setPlayerMark(player, mark, value);
}

void Room::setPlayerCardLimitation(ServerPlayer *player, const QString &limit_list,
    const QString &pattern, bool single_turn)
{
    player->setCardLimitation(limit_list, pattern, single_turn);

    JsonArray arg;
    arg << true;
    arg << limit_list;
    arg << pattern;
    arg << single_turn;
    doNotify(player, S_COMMAND_CARD_LIMITATION, arg);
}

void Room::removePlayerCardLimitation(ServerPlayer *player, const QString &limit_list,
    const QString &pattern)
{
    player->removeCardLimitation(limit_list, pattern);

    JsonArray arg;
    arg << false;
    arg << limit_list;
    arg << pattern;
    arg << false;
    doNotify(player, S_COMMAND_CARD_LIMITATION, arg);
}

void Room::clearPlayerCardLimitation(ServerPlayer *player, bool single_turn)
{
    player->clearCardLimitation(single_turn);

    JsonArray arg;
    arg << true;
    arg << QVariant();
    arg << QVariant();
    arg << single_turn;
    doNotify(player, S_COMMAND_CARD_LIMITATION, arg);
}

void Room::setCardFlag(const Card *card, const QString &flag, ServerPlayer *who)
{
    if (flag.isEmpty()) return;

    card->setFlags(flag);

    if (!card->isVirtualCard())
        setCardFlag(card->getEffectiveId(), flag, who);
}

void Room::setCardFlag(int card_id, const QString &flag, ServerPlayer *who)
{
    if (flag.isEmpty()) return;

    Sanguosha->getCard(card_id)->setFlags(flag);

    JsonArray arg;
    arg << card_id;
    arg << flag;
    if (who)
        doNotify(who, S_COMMAND_CARD_FLAG, arg);
    else
        doBroadcastNotify(S_COMMAND_CARD_FLAG, arg);
}

void Room::clearCardFlag(const Card *card, ServerPlayer *who)
{
    card->clearFlags();

    if (!card->isVirtualCard())
        clearCardFlag(card->getEffectiveId(), who);
}

void Room::clearCardFlag(int card_id, ServerPlayer *who)
{
    Sanguosha->getCard(card_id)->clearFlags();

    JsonArray arg;
    arg << card_id;
    arg << ".";
    if (who)
        doNotify(who, S_COMMAND_CARD_FLAG, arg);
    else
        doBroadcastNotify(S_COMMAND_CARD_FLAG, arg);
}

ServerPlayer *Room::addSocket(ClientSocket *socket)
{
    ServerPlayer *player = new ServerPlayer(this);
    player->setSocket(socket);
    m_players << player;

    connect(player, SIGNAL(disconnected()), this, SLOT(reportDisconnection()));
    connect(player, SIGNAL(request_got(QString)), this, SLOT(processClientPacket(QString)));

    return player;
}

bool Room::isFull() const
{
    return m_players.length() == player_count;
}

bool Room::isFinished() const
{
    return game_finished;
}

bool Room::canPause(ServerPlayer *player) const
{
    if (!isFull()) return false;
    if (!player || !player->isOwner()) return false;
    foreach (ServerPlayer *p, m_players) {
        if (!p->isAlive() || p->isOwner()) continue;
        if (p->getState() != "robot")
            return false;
    }
    return true;
}

void Room::tryPause()
{
    if (!canPause(getOwner())) return;
    QMutexLocker locker(&m_mutex);
    while (game_paused)
        m_waitCond.wait(locker.mutex());
}

int Room::getLack() const
{
    return player_count - m_players.length();
}

QString Room::getMode() const
{
    return mode;
}

const Scenario *Room::getScenario() const
{
    return scenario;
}

void Room::broadcast(const QString &message, ServerPlayer *except)
{
    foreach (ServerPlayer *player, m_players) {
        if (player != except)
            player->unicast(message);
    }
}

void Room::swapPile()
{
    if (m_discardPile->isEmpty()) {
        // the standoff
        gameOver(".");
    }

    int times = tag.value("SwapPile", 0).toInt();
    tag.insert("SwapPile", ++times);

    int limit = Config.value("PileSwappingLimitation", 5).toInt() + 1;
    if (mode == "04_1v3")
        limit = qMin(limit, Config.BanPackages.contains("maneuvering") ? 3 : 2);
    else if (mode == "08_defense")
        limit = qMin(limit, Config.BanPackages.contains("maneuvering") ? 9 : 6);
    if (limit > 0 && times == limit)
        gameOver(".");

    qSwap(m_drawPile, m_discardPile);

    doBroadcastNotify(S_COMMAND_RESET_PILE, QVariant());
    doBroadcastNotify(S_COMMAND_UPDATE_PILE, m_drawPile->length());

    qShuffle(*m_drawPile);
    foreach(int card_id, *m_drawPile)
        setCardMapping(card_id, NULL, Player::DrawPile);
}

ServerPlayer *Room::findPlayer(const QString &general_name, bool include_dead) const
{
    const QList<ServerPlayer *> &list = include_dead ? m_players : m_alivePlayers;

    if (general_name.contains("+")) {
        QStringList names = general_name.split("+");
        foreach (ServerPlayer *player, list) {
            if (names.contains(player->getGeneralName()))
                return player;
        }
        return NULL;
    }

    foreach (ServerPlayer *player, list) {
        if (player->getGeneralName() == general_name)
            return player;
    }

    return NULL;
}

QList<ServerPlayer *>Room::findPlayersBySkillName(const QString &skill_name) const
{
    QList<ServerPlayer *> list;
    foreach (ServerPlayer *player, getAllPlayers()) {
        if (player->hasSkill(skill_name))
            list << player;
    }
    return list;
}

ServerPlayer *Room::findPlayerBySkillName(const QString &skill_name) const
{
    foreach (ServerPlayer *player, getAllPlayers()) {
        if (player->hasSkill(skill_name))
            return player;
    }
    return NULL;
}

void Room::installEquip(ServerPlayer *player, const QString &equip_name)
{
    if (player == NULL) return;

    int card_id = getCardFromPile(equip_name);
    if (card_id == -1) return;

    moveCardTo(Sanguosha->getCard(card_id), player, Player::PlaceEquip, true);
}

void Room::resetAI(ServerPlayer *player)
{
    AI *smart_ai = player->getSmartAI();
    int index = -1;
    if (smart_ai) {
        index = ais.indexOf(smart_ai);
        ais.removeOne(smart_ai);
        delete smart_ai;
    }
    AI *new_ai = cloneAI(player);
    player->setAI(new_ai);
    if (index == -1)
        ais.append(new_ai);
    else
        ais.insert(index, new_ai);
}

void Room::changeHero(ServerPlayer *player, const QString &new_general, bool full_state, bool invokeStart,
    bool isSecondaryHero, bool sendLog)
{
    JsonArray arg;
    arg << (int)S_GAME_EVENT_CHANGE_HERO;
    arg << player->objectName();
    arg << new_general;
    arg << isSecondaryHero;
    arg << sendLog;
    doBroadcastNotify(QSanProtocol::S_COMMAND_LOG_EVENT, arg);

    if (isSecondaryHero)
        changePlayerGeneral2(player, new_general);
    else
        changePlayerGeneral(player, new_general);
    player->setMaxHp(player->getGeneralMaxHp());

    if (full_state)
        player->setHp(player->getMaxHp());
    broadcastProperty(player, "hp");
    broadcastProperty(player, "maxhp");

    QVariant void_data;
    QList<const TriggerSkill *> game_start;
    const General *gen = isSecondaryHero ? player->getGeneral2() : player->getGeneral();
    if (gen) {
        foreach (const Skill *skill, gen->getSkillList()) {
            if (skill->inherits("TriggerSkill")) {
                const TriggerSkill *trigger = qobject_cast<const TriggerSkill *>(skill);
                thread->addTriggerSkill(trigger);
                if (invokeStart && trigger->getTriggerEvents().contains(GameStart) && trigger->triggerable(player, this))
                    game_start << trigger;
            }
            if (skill->getFrequency() == Skill::Limited && !skill->getLimitMark().isEmpty())
                setPlayerMark(player, skill->getLimitMark(), 1);

            QVariant _skillobjectName = skill->objectName();
            thread->trigger(EventAcquireSkill, this, player, _skillobjectName);
        }
    }
    if (invokeStart) {
        foreach(const TriggerSkill *skill, game_start)
            skill->trigger(GameStart, this, player, void_data);
    }
    resetAI(player);
}

lua_State *Room::getLuaState() const
{
    return L;
}

void Room::setFixedDistance(Player *from, const Player *to, int distance)
{
    from->setFixedDistance(to, distance);

    JsonArray arg;
    arg << from->objectName();
    arg << to->objectName();
    arg << distance;
    arg << true;
    doBroadcastNotify(S_COMMAND_FIXED_DISTANCE, arg);
}

void Room::removeFixedDistance(Player *from, const Player *to, int distance)
{
    from->removeFixedDistance(to, distance);

    JsonArray arg;
    arg << from->objectName();
    arg << to->objectName();
    arg << distance;
    arg << false;
    doBroadcastNotify(S_COMMAND_FIXED_DISTANCE, arg);
}

void Room::insertAttackRangePair(Player *from, const Player *to)
{
    from->insertAttackRangePair(to);

    JsonArray arg;
    arg << from->objectName();
    arg << to->objectName();
    arg << true;
    doBroadcastNotify(S_COMMAND_ATTACK_RANGE, arg);
}

void Room::removeAttackRangePair(Player *from, const Player *to)
{
    from->removeAttackRangePair(to);

    JsonArray arg;
    arg << from->objectName();
    arg << to->objectName();
    arg << false;
    doBroadcastNotify(S_COMMAND_ATTACK_RANGE, arg);
}

void Room::reverseFor3v3(const Card *card, ServerPlayer *player, QList<ServerPlayer *> &list)
{
    tryPause();
    notifyMoveFocus(player, S_COMMAND_CHOOSE_DIRECTION);

    bool isClockwise = false;
    if (player->isOnline()) {
        bool success = doRequest(player, S_COMMAND_CHOOSE_DIRECTION, QVariant(), true);
        QVariant clientReply = player->getClientReply();
        if (success && JsonUtils::isString(clientReply))
            isClockwise = (clientReply.toString() == "cw");
    } else {
        QVariant data = QVariant::fromValue(card);
        isClockwise = (askForChoice(player, "3v3_direction", "cw+ccw", data) == "cw");
    }

    LogMessage log;
    log.type = "#TrickDirection";
    log.from = player;
    log.arg = isClockwise ? "cw" : "ccw";
    log.arg2 = card->objectName();
    sendLog(log);

    if (isClockwise) {
        QList<ServerPlayer *> new_list;

        while (!list.isEmpty())
            new_list << list.takeLast();

        if (card->isKindOf("GlobalEffect")) {
            new_list.removeLast();
            new_list.prepend(player);
        }

        list = new_list;
    }
}

const ProhibitSkill *Room::isProhibited(const Player *from, const Player *to, const Card *card, const QList<const Player *> &others) const
{
    return Sanguosha->isProhibited(from, to, card, others);
}

int Room::drawCard()
{
    thread->trigger(FetchDrawPileCard, this, NULL);
    if (m_drawPile->isEmpty())
        swapPile();
    return m_drawPile->takeFirst();
}

void Room::prepareForStart()
{
    if (scenario) {
        QStringList generals, roles;
        scenario->assign(generals, roles);

        bool expose_roles = scenario->exposeRoles();
        for (int i = 0; i < m_players.length(); i++) {
            ServerPlayer *player = m_players[i];
            if (generals.size() > i && !generals[i].isNull()) {
                player->setGeneralName(generals[i]);
                broadcastProperty(player, "general");
            }

            player->setRole(roles.at(i));
            if (player->isLord())
                broadcastProperty(player, "role");

            if (expose_roles)
                broadcastProperty(player, "role");
            else
                notifyProperty(player, player, "role");
        }
    } 
    else if (mode == "06_3v3" || mode == "06_XMode" || mode == "02_1v1") {
        return;
    }
    else if (!Config.EnableHegemony && Config.EnableCheat && Config.value("FreeAssign", false).toBool()) {
        ServerPlayer *owner = getOwner();
        notifyMoveFocus(owner, S_COMMAND_CHOOSE_ROLE);
        if (owner && owner->isOnline()) {
            bool success = doRequest(owner, S_COMMAND_CHOOSE_ROLE, QVariant(), true);
            QVariant clientReply = owner->getClientReply();
            if (!success || !clientReply.canConvert<JsonArray>() || clientReply.value<JsonArray>().size() != 2) {
                if (Config.RandomSeat)
                    qShuffle(m_players);
                assignRoles();
            } else if (Config.FreeAssignSelf) {
                JsonArray replyArray = clientReply.value<JsonArray>();
                QString name = replyArray.value(0).value<JsonArray>().value(0).toString();
                QString role = replyArray.value(1).value<JsonArray>().value(0).toString();
                ServerPlayer *player_self = findChild<ServerPlayer *>(name);
                setPlayerProperty(player_self, "role", role);

                QList<ServerPlayer *> all_players = m_players;
                all_players.removeOne(player_self);
                int n = all_players.count();
                QStringList roles = Sanguosha->getRoleList(mode);
                roles.removeOne(role);
                qShuffle(roles);

                for (int i = 0; i < n; i++) {
                    ServerPlayer *player = all_players[i];
                    QString role = roles.at(i);

                    player->setRole(role);
                    if (role == "lord" && !ServerInfo.EnableHegemony)
                        broadcastProperty(player, "role", "lord");
                    else {
                        if (mode == "04_1v3" || mode == "04_boss" || mode == "08_defense")
                            broadcastProperty(player, "role", role);
                        else
                            notifyProperty(player, player, "role");
                    }
                }
            } else {
                JsonArray replyArray = clientReply.value<JsonArray>();
                QString name = replyArray.value(0).value<JsonArray>().value(0).toString();
                QString role = replyArray.value(1).value<JsonArray>().value(0).toString();
                for (int i = 0; i < replyArray.value(0).value<JsonArray>().size(); i++) {
                    QString name = replyArray.value(0).value<JsonArray>().value(i).toString();
                    QString role = replyArray.value(1).value<JsonArray>().value(i).toString();

                    ServerPlayer *player = findChild<ServerPlayer *>(name);
                    setPlayerProperty(player, "role", role);

                    m_players.swap(i, m_players.indexOf(player));
                }
            }
        } else if (mode == "04_1v3" || mode == "04_boss") {
            if (Config.RandomSeat)
                qShuffle(m_players);
            ServerPlayer *lord = m_players.at(qrand() % 4);
            for (int i = 0; i < 4; i++) {
                ServerPlayer *player = m_players.at(i);
                if (player == lord)
                    player->setRole("lord");
                else
                    player->setRole("rebel");
                broadcastProperty(player, "role");
            }
        } else {
            if (Config.RandomSeat)
                qShuffle(m_players);
            assignRoles();
        }
    } 
    else {
        if (Config.RandomSeat)
            qShuffle(m_players);
        assignRoles();
    }

    adjustSeats();
}

void Room::reportDisconnection()
{
    ServerPlayer *player = qobject_cast<ServerPlayer *>(sender());
    if (player == NULL) return;

    // send disconnection message to server log
    emit room_message(player->reportHeader() + tr("disconnected"));

    // the 4 kinds of circumstances
    // 1. Just connected, with no object name : just remove it from player list
    // 2. Connected, with an object name : remove it, tell other clients and decrease signup_count
    // 3. Game is not started, but role is assigned, give it the default general(general2) and others same with fourth case
    // 4. Game is started, do not remove it just set its state as offline
    // all above should set its socket to NULL

    player->setSocket(NULL);

    if (player->objectName().isEmpty()) {
        // first case
        player->setParent(NULL);
        m_players.removeOne(player);
    } else if (player->getRole().isEmpty()) {
        // second case
        if (m_players.length() < player_count) {
            player->setParent(NULL);
            m_players.removeOne(player);

            if (player->getState() != "robot") {
                QString screen_name = player->screenName();
                QString leaveStr = tr("<font color=#000000>Player <b>%1</b> left the game</font>").arg(screen_name);
                speakCommand(player, QString(leaveStr.toUtf8().toBase64()));
            }

            doBroadcastNotify(S_COMMAND_REMOVE_PLAYER, player->objectName());
        }
    } else {
        // fourth case
        if (player->m_isWaitingReply)
            player->releaseLock(ServerPlayer::SEMA_COMMAND_INTERACTIVE);
        setPlayerProperty(player, "state", "offline");

        bool someone_is_online = false;
        foreach (ServerPlayer *player, m_players) {
            if (player->getState() == "online" || player->getState() == "trust") {
                someone_is_online = true;
                break;
            }
        }

        if (!someone_is_online) {
            game_finished = true;
            emit game_over(QString());
            return;
        }
    }

    if (player->isOwner()) {
        player->setOwner(false);
        broadcastProperty(player, "owner");
        foreach (ServerPlayer *p, m_players) {
            if (p->getState() == "online") {
                p->setOwner(true);
                broadcastProperty(p, "owner");
                break;
            }
        }
    }
}

void Room::trustCommand(ServerPlayer *player, const QVariant &)
{
    player->acquireLock(ServerPlayer::SEMA_MUTEX);
    if (player->isOnline()) {
        player->setState("trust");
        if (player->m_isWaitingReply) {
            player->releaseLock(ServerPlayer::SEMA_MUTEX);
            player->releaseLock(ServerPlayer::SEMA_COMMAND_INTERACTIVE);
        }
    } else
        player->setState("online");

    player->releaseLock(ServerPlayer::SEMA_MUTEX);
    broadcastProperty(player, "state");
    return;
}

void Room::pauseCommand(ServerPlayer *player, const QVariant &arg)
{
    if (!canPause(player))
        return;
    bool pause = arg.toBool();
    QMutexLocker locker(&m_mutex);
    if (game_paused != pause) {
        JsonArray arg;
        arg << S_GAME_EVENT_PAUSE << pause;
        doNotify(player, S_COMMAND_LOG_EVENT, arg);

        game_paused = pause;
        if (!game_paused)
            m_waitCond.wakeAll();
    }
    return;
}

void Room::processRequestCheat(ServerPlayer *player, const QVariant &arg)
{
    player->m_cheatArgs = QVariant();
    if (!Config.EnableCheat)
        return;
    if (!arg.canConvert<JsonArray>() || !arg.value<JsonArray>().value(0).canConvert(QVariant::Int))
        return;
    //@todo: synchronize this
    player->m_cheatArgs = arg;
    player->releaseLock(ServerPlayer::SEMA_COMMAND_INTERACTIVE);
    return;
}

bool Room::makeSurrender(ServerPlayer *initiator)
{
    bool loyalGiveup = true;
    int loyalAlive = 0;
    bool renegadeGiveup = true;
    int renegadeAlive = 0;
    bool rebelGiveup = true;
    int rebelAlive = 0;

    // broadcast polling request
    QList<ServerPlayer *> playersAlive;
    foreach (ServerPlayer *player, m_players) {
        QString playerRole = player->getRole();
        if ((playerRole == "loyalist" || playerRole == "lord") && player->isAlive()) loyalAlive++;
        else if (playerRole == "rebel" && player->isAlive()) rebelAlive++;
        else if (playerRole == "renegade" && player->isAlive()) renegadeAlive++;

        if (player != initiator && player->isAlive() && player->isOnline()) {
            player->m_commandArgs = (initiator->getGeneral()->objectName());
            playersAlive << player;
        }
    }
    doBroadcastRequest(playersAlive, S_COMMAND_SURRENDER);

    // collect polls
    foreach (ServerPlayer *player, playersAlive) {
        bool result = false;
        if (!player->m_isClientResponseReady || !player->getClientReply().canConvert(QVariant::Bool))
            result = !player->isOnline();
        else
            result = player->getClientReply().toBool();

        QString playerRole = player->getRole();
        if (playerRole == "loyalist" || playerRole == "lord") {
            loyalGiveup &= result;
            if (player->isAlive()) loyalAlive++;
        } else if (playerRole == "rebel") {
            rebelGiveup &= result;
            if (player->isAlive()) rebelAlive++;
        } else if (playerRole == "renegade") {
            renegadeGiveup &= result;
            if (player->isAlive()) renegadeAlive++;
        }
    }

    // vote counting
    if (loyalGiveup && renegadeGiveup && !rebelGiveup)
        gameOver("rebel");
    else if (loyalGiveup && !renegadeGiveup && rebelGiveup)
        gameOver("renegade");
    else if (!loyalGiveup && renegadeGiveup && rebelGiveup)
        gameOver("lord+loyalist");
    else if (loyalGiveup && renegadeGiveup && rebelGiveup) {
        // if everyone give up, then ensure that the initiator doesn't win.
        QString playerRole = initiator->getRole();
        if (playerRole == "lord" || playerRole == "loyalist")
            gameOver(renegadeAlive >= rebelAlive ? "renegade" : "rebel");
        else if (playerRole == "renegade")
            gameOver(loyalAlive >= rebelAlive ? "loyalist+lord" : "rebel");
        else if (playerRole == "rebel")
            gameOver(renegadeAlive >= loyalAlive ? "renegade" : "loyalist+lord");
    }

    m_surrenderRequestReceived = false;

    initiator->setFlags("Global_ForbidSurrender");
    doNotify(initiator, S_COMMAND_ENABLE_SURRENDER, QVariant(false));
    return true;
}

void Room::processRequestSurrender(ServerPlayer *player, const QVariant &)
{
    //@todo: Strictly speaking, the client must be in the PLAY phase
    //@todo: return false for 3v3 and 1v1!!!
    if (player == NULL || !player->m_isWaitingReply)
        return;
    if (!_m_isFirstSurrenderRequest
        && _m_timeSinceLastSurrenderRequest.elapsed() <= Config.S_SURRENDER_REQUEST_MIN_INTERVAL)
        return; //@todo: warn client here after new protocol has been enacted on the warn request

    _m_isFirstSurrenderRequest = false;
    _m_timeSinceLastSurrenderRequest.restart();
    m_surrenderRequestReceived = true;
    player->releaseLock(ServerPlayer::SEMA_COMMAND_INTERACTIVE);
    return;
}

void Room::processClientPacket(const QString &request)
{
    Packet packet;
    if (packet.parse(request.toLatin1().constData())) {
        ServerPlayer *player = qobject_cast<ServerPlayer *>(sender());
#ifdef LOGNETWORK
        emit Sanguosha->logNetworkMessage("recv "+player->objectName()+":"+request);
#endif // LOGNETWORK
        if (game_finished) {
            if (player && player->isOnline())
                doNotify(player, S_COMMAND_WARN, QString("GAME_OVER"));
            return;
        }
        if (packet.getPacketType() == S_TYPE_REPLY) {
            if (player == NULL) return;
            player->setClientReplyString(request);
            processResponse(player, &packet);
        } else if (packet.getPacketType() == S_TYPE_REQUEST || packet.getPacketType() == S_TYPE_NOTIFICATION) {
            Callback callback = m_callbacks[packet.getCommandType()];
            if (!callback) return;
            (this->*callback)(player, packet.getMessageBody());
        }
    }
}

void Room::addRobotCommand(ServerPlayer *player, const QVariant &arg)
{
    if ((player && !player->isOwner()) || !arg.canConvert(QVariant::Int))
        return;

    int n = 0;
    foreach (ServerPlayer *player, m_players) {
        if (player->getState() == "robot")
            n++;
    }

    int add_num = arg.toInt();
    if (add_num == -1)
        add_num = player_count - m_players.length();

    for (int i = 0; i < add_num; i++) {
        if (isFull())
            return;
        ServerPlayer *robot = new ServerPlayer(this);
        robot->setState("robot");

        m_players << robot;

        const QString robot_name = tr("Computer %1").arg(QChar('A' + n));
        n++;
        const QString robot_avatar = Sanguosha->getRandomGeneralName();
        signup(robot, robot_name, robot_avatar, true);

        QString greeting = tr("Hello, I'm a robot").toUtf8().toBase64();
        speakCommand(robot, greeting);

        broadcastProperty(robot, "state");
    }
}

ServerPlayer *Room::getOwner() const
{
    foreach (ServerPlayer *player, m_players) {
        if (player->isOwner())
            return player;
    }

    return NULL;
}

void Room::toggleReadyCommand(ServerPlayer *, const QVariant &)
{
    if (!game_started && isFull())
        start();
}

void Room::signup(ServerPlayer *player, const QString &screen_name, const QString &avatar, bool is_robot)
{
    player->setObjectName(generatePlayerName());
    player->setProperty("avatar", avatar);
    player->setScreenName(screen_name);

    if (!is_robot) {
        notifyProperty(player, player, "objectName");

        ServerPlayer *owner = getOwner();
        if (owner == NULL) {
            player->setOwner(true);
            notifyProperty(player, player, "owner");
        }
    }

    // introduce the new joined player to existing players except himself
    player->introduceTo(NULL);

    if (!is_robot) {
        QString greetingStr = tr("<font color=#EEB422>Player <b>%1</b> joined the game</font>").arg(screen_name);
        speakCommand(player, QString(greetingStr.toUtf8().toBase64()));
        player->startNetworkDelayTest();

        // introduce all existing player to the new joined
        foreach (ServerPlayer *p, m_players) {
            if (p != player)
                p->introduceTo(player);
        }
    } else
        toggleReadyCommand(player, QVariant());
}

void Room::assignGeneralsForPlayers(const QList<ServerPlayer *> &to_assign)
{
    QSet<QString> existed;
    foreach (ServerPlayer *player, m_players) {
        if (player->getGeneral())
            existed << player->getGeneralName();
        if (player->getGeneral2())
            existed << player->getGeneral2Name();
    }
    if (Config.Enable2ndGeneral) {
        foreach(QString name, BanPair::getAllBanSet())
            existed << name;
        if (to_assign.first()->getGeneral()) {
            foreach(QString name, BanPair::getSecondBanSet())
                existed << name;
        }
    }

    const int max_choice = (Config.EnableHegemony && Config.Enable2ndGeneral) ?
        Config.value("HegemonyMaxChoice", 7).toInt() :
        Config.value("MaxChoice", 5).toInt();
    const int total = Sanguosha->getGeneralCount();
    const int max_available = (total - existed.size()) / to_assign.length();
    const int choice_count = qMin(max_choice, max_available);

    QStringList choices = Sanguosha->getRandomGenerals(total - existed.size(), existed);

    if (Config.EnableHegemony) {
        if (to_assign.first()->getGeneral()) {
            foreach (ServerPlayer *sp, m_players) {
                QStringList old_list = sp->getSelected();
                sp->clearSelected();
                QString choice;

                //keep legal generals
                foreach (QString name, old_list) {
                    if (Sanguosha->getGeneral(name)->getKingdom() != sp->getGeneral()->getKingdom()
                        || sp->findReasonable(old_list, true) == name) {
                        sp->addToSelected(name);
                        old_list.removeOne(name);
                    }
                }

                //drop the rest and add new generals
                while (old_list.length()) {
                    choice = sp->findReasonable(choices);
                    sp->addToSelected(choice);
                    old_list.pop_front();
                    choices.removeOne(choice);
                }
            }
            return;
        }
    }

    foreach (ServerPlayer *player, to_assign) {
        player->clearSelected();

        for (int i = 0; i < choice_count; i++) {
            QString choice = player->findReasonable(choices, true);
            if (choice.isEmpty()) break;
            player->addToSelected(choice);
            choices.removeOne(choice);
        }
    }
}

void Room::assignGeneralsForPlayersOfJianGeDefenseMode(const QList<ServerPlayer *> &to_assign)
{
    QMap<QString, QSet<QString> > existed;
    foreach (ServerPlayer *player, m_players) {
        if (player->property("jiange_defense_type").toString() != "general")
            continue;
        if (player->getGeneral())
            existed[player->getGeneral()->getKingdom()] << player->getGeneralName();
        if (player->getGeneral2())
            existed[player->getGeneral2()->getKingdom()] << player->getGeneral2Name();
    }
    if (Config.Enable2ndGeneral) {
        foreach(QString name, BanPair::getAllBanSet())
        {
            const General *gen = Sanguosha->getGeneral(name);
            if (gen)
                existed[gen->getKingdom()] << name;
        }
        if (to_assign.first()->getGeneral()) {
            foreach(QString name, BanPair::getSecondBanSet())
            {
                const General *gen = Sanguosha->getGeneral(name);
                if (gen)
                    existed[gen->getKingdom()] << name;
            }
        }
    }

    const int max_choice = Config.value("MaxChoice", 5).toInt();
    QMap<QString, QStringList> general_choices;
    foreach (QString key, Config.JianGeDefenseKingdoms.keys()) {
        QString kingdom = Config.JianGeDefenseKingdoms[key];
        int total = Sanguosha->getGeneralCount(false, kingdom);
        general_choices[kingdom] = Sanguosha->getRandomGenerals(total - existed[kingdom].size(), existed[kingdom], kingdom);
    }

    foreach (ServerPlayer *player, to_assign) {
        QStringList choices;
        int choice_count = 0;
        QString kingdom = Config.JianGeDefenseKingdoms[player->getRole()];
        QString jiange_defense_type = player->property("jiange_defense_type").toString();
        if (jiange_defense_type == "general") {
            int total = Sanguosha->getGeneralCount(false, kingdom);
            int max_available = (total - existed[kingdom].size()) / 2;
            choice_count = qMin(max_choice, max_available);
            choices = general_choices[kingdom];
        } else if (jiange_defense_type == "machine") {
            choices = Config.JianGeDefenseMachine[kingdom];
            choice_count = choices.length();
        } else if (jiange_defense_type == "soul") {
            choices = Config.JianGeDefenseSoul[kingdom];
            choice_count = choices.length();
        } else {
            Q_ASSERT(false);
        }

        player->clearSelected();

        for (int i = 0; i < choice_count; i++) {
            QString choice = player->findReasonable(choices, true);
            if (choice.isEmpty()) break;
            player->addToSelected(choice);
            choices.removeOne(choice);
            if (jiange_defense_type == "general")
                general_choices[kingdom].removeOne(choice);
        }
    }
}

void Room::chooseGenerals(QList<ServerPlayer *> players)
{
    if (players.isEmpty()) players = m_players;
    // for lord.
    int lord_num = Config.value("LordMaxChoice", -1).toInt();
    int nonlord_num = Config.value("NonLordMaxChoice", 2).toInt();
    if (lord_num == 0 && nonlord_num == 0)
        nonlord_num = 1;
    int nonlord_prob = (lord_num == -1) ? 5 : 55 - qMin(lord_num, 10);
    ServerPlayer *the_lord = getLord();
    if (!Config.EnableHegemony && the_lord && players.contains(the_lord)) {
        QStringList lord_list;
        if (Config.EnableSame)
            lord_list = Sanguosha->getRandomGenerals(Config.value("MaxChoice", 5).toInt());
        else if (the_lord->getState() == "robot")
            if (((qrand() % 100 < nonlord_prob || lord_num == 0) && nonlord_num > 0)
                || Sanguosha->getLords().length() == 0)
                lord_list = Sanguosha->getRandomGenerals(1);
            else
                lord_list = Sanguosha->getLords();
        else
            lord_list = Sanguosha->getRandomLords();
        QString general = askForGeneral(the_lord, lord_list);
        the_lord->setGeneralName(general);
        if (!Config.EnableBasara)
            broadcastProperty(the_lord, "general", general);

        if (Config.EnableSame) {
            foreach (ServerPlayer *p, players) {
                if (!p->isLord())
                    p->setGeneralName(general);
            }

            Config.Enable2ndGeneral = false;
            return;
        }
    }
    QList<ServerPlayer *> to_assign = players;
    if (the_lord && !Config.EnableHegemony) to_assign.removeOne(the_lord);

    assignGeneralsForPlayers(to_assign);
    foreach(ServerPlayer *player, to_assign)
        _setupChooseGeneralRequestArgs(player);

    doBroadcastRequest(to_assign, S_COMMAND_CHOOSE_GENERAL);
    foreach (ServerPlayer *player, to_assign) {
        if (player->getGeneral() != NULL) continue;
        QString generalName = player->getClientReply().toString();
        if (!player->m_isClientResponseReady ||  !_setPlayerGeneral(player, generalName, true))
            _setPlayerGeneral(player, _chooseDefaultGeneral(player), true);
    }

    if (Config.Enable2ndGeneral) {
        QList<ServerPlayer *> to_assign = players;
        assignGeneralsForPlayers(to_assign);
        foreach(ServerPlayer *player, to_assign)
            _setupChooseGeneralRequestArgs(player);

        doBroadcastRequest(to_assign, S_COMMAND_CHOOSE_GENERAL);
        foreach (ServerPlayer *player, to_assign) {
            if (player->getGeneral2() != NULL) continue;
            QString generalName = player->getClientReply().toString();
            if (!player->m_isClientResponseReady || !_setPlayerGeneral(player, generalName, false))
                _setPlayerGeneral(player, _chooseDefaultGeneral(player), false);
        }
    }

    if (Config.EnableBasara) {
        foreach (ServerPlayer *player, m_players) {
            QStringList names;
            if (player->getGeneral()) {
                QString name = player->getGeneralName();
                names.append(name);
                player->setGeneralName("anjiang");
                notifyProperty(player, player, "general");
            }
            if (player->getGeneral2() && Config.Enable2ndGeneral) {
                QString name = player->getGeneral2Name();
                names.append(name);
                player->setGeneral2Name("anjiang");
                notifyProperty(player, player, "general2");
            }
            player->setProperty("basara_generals", names.join("+"));
            notifyProperty(player, player, "basara_generals");
        }
    }
}

void Room::chooseGeneralsOfJianGeDefenseMode()
{
    QList<ServerPlayer *> to_assign = m_players;

    assignGeneralsForPlayersOfJianGeDefenseMode(to_assign);
    foreach(ServerPlayer *player, to_assign)
        _setupChooseGeneralRequestArgs(player);

    doBroadcastRequest(to_assign, S_COMMAND_CHOOSE_GENERAL);
    foreach (ServerPlayer *player, to_assign) {
        if (player->getGeneral() != NULL) continue;
        QString generalName = player->getClientReply().toString();
        if (!player->m_isClientResponseReady || !_setPlayerGeneral(player, generalName, true)) {
            QString result = _chooseDefaultGeneral(player);
            if (player->property("jiange_defense_type").toString() != "general") { // randomly chosen
                QStringList selected = player->getSelected();
                result = selected.at(qrand() % selected.length());
            }
            _setPlayerGeneral(player, result, true);
        }
    }

    if (Config.Enable2ndGeneral) {
        QList<ServerPlayer *> to_assign;
        foreach (ServerPlayer *p, m_players) {
            if (p->property("jiange_defense_type").toString() == "general")
                to_assign << p;
        }
        assignGeneralsForPlayersOfJianGeDefenseMode(to_assign);
        foreach(ServerPlayer *player, to_assign)
            _setupChooseGeneralRequestArgs(player);

        doBroadcastRequest(to_assign, S_COMMAND_CHOOSE_GENERAL);
        foreach (ServerPlayer *player, to_assign) {
            if (player->getGeneral2() != NULL) continue;
            QString generalName = player->getClientReply().toString();
            if (!player->m_isClientResponseReady || !_setPlayerGeneral(player, generalName, false)) {
                _setPlayerGeneral(player, _chooseDefaultGeneral(player), false);
            }
        }
    }
}

void Room::run()
{
    // initialize random seed for later use
    qsrand(QTime(0, 0, 0).secsTo(QTime::currentTime()));
    Config.AIDelay = Config.OriginAIDelay;

    foreach (ServerPlayer *player, m_players) {
        //Ensure that the game starts with all player's mutex locked
        player->drainAllLocks();
        player->releaseLock(ServerPlayer::SEMA_MUTEX);
    }

    prepareForStart();

    bool using_countdown = true;
    if (_virtual || !property("to_test").toString().isEmpty())
        using_countdown = false;

#ifndef QT_NO_DEBUG
    using_countdown = false;
#endif

    if (using_countdown) {
        for (int i = Config.CountDownSeconds; i >= 0; i--) {
            doBroadcastNotify(S_COMMAND_START_IN_X_SECONDS, i);
            sleep(1);
        }
    }
    else
        doBroadcastNotify(S_COMMAND_START_IN_X_SECONDS, QVariant(0));

    if (scenario && !scenario->generalSelection())
        startGame();
    else if (mode == "06_3v3") {
        thread_3v3 = new RoomThread3v3(this);
        thread_3v3->start();

        connect(thread_3v3, SIGNAL(finished()), this, SLOT(startGame()));
        connect(thread_3v3, SIGNAL(finished()), thread_3v3, SLOT(deleteLater()));
    } 
    else if (mode == "06_XMode") {
        thread_xmode = new RoomThreadXMode(this);
        thread_xmode->start();

        connect(thread_xmode, SIGNAL(finished()), this, SLOT(startGame()));
        connect(thread_xmode, SIGNAL(finished()), thread_xmode, SLOT(deleteLater()));
    }
    else if (mode == "02_1v1") {
        thread_1v1 = new RoomThread1v1(this);
        thread_1v1->start();

        connect(thread_1v1, SIGNAL(finished()), this, SLOT(startGame()));
        connect(thread_1v1, SIGNAL(finished()), thread_1v1, SLOT(deleteLater()));
    } 
    else if (mode == "04_1v3") {
        ServerPlayer *lord = m_players.first();
        setPlayerProperty(lord, "general", "shenlvbu1");

        QStringList names;
        foreach (QString gen_name, GetConfigFromLuaState(Sanguosha->getLuaState(), "hulao_generals").toStringList()) {
            if (gen_name.startsWith("-")) { // means banned generals
                names.removeOne(gen_name.mid(1));
            } else if (gen_name.startsWith("package:")) {
                QString pack_name = gen_name.mid(8);
                const Package *pack = Sanguosha->findChild<const Package *>(pack_name);
                if (pack) {
                    foreach(const General *general, pack->findChildren<const General *>())
                    {
                        if (general->isTotallyHidden())
                            continue;
                        if (!names.contains(general->objectName()))
                            names << general->objectName();
                    }
                }
            } else if (!names.contains(gen_name)) {
                names << gen_name;
            }
        }

        foreach (ServerPlayer *player, m_players) {
            if (player == lord)
                continue;

            qShuffle(names);
            QStringList choices = names.mid(0, 3);
            QString name = askForGeneral(player, choices);

            setPlayerProperty(player, "general", name);
            names.removeOne(name);
        }

        startGame();
    } 
    else if (mode == "04_boss")
    {
        ServerPlayer *lord = m_players.first();
        QStringList boss_lv_1 = Config.BossGenerals.first().split("+");
        if (Config.value("OptionalBoss", false).toBool()) {
            QString gen = askForGeneral(lord, boss_lv_1);
            setPlayerProperty(lord, "general", gen);
        } else {
            setPlayerProperty(lord, "general", boss_lv_1.at(qrand() % 4));
        }
        setPlayerMark(lord, "BossMode_Boss", 1);

        QList<ServerPlayer *> players = m_players;
        players.removeOne(lord);
        chooseGenerals(players);
        startGame();
    } 
    else if (mode == "08_defense") {
        QStringList type_list;
        type_list << "machine" << "general" << "soul" << "general"
            << "general" << "soul" << "general" << "machine";
        for (int i = 0; i < 8; i++)
            setPlayerProperty(m_players.at(i), "jiange_defense_type", type_list.at(i));
        chooseGeneralsOfJianGeDefenseMode();
        startGame();
    } 
    else {
        chooseGenerals();
        startGame();
    }
}

void Room::assignRoles()
{
    int n = m_players.count();

    QStringList roles = Sanguosha->getRoleList(mode);
    if (mode != "08_defense")
        qShuffle(roles);

    for (int i = 0; i < n; i++) {
        ServerPlayer *player = m_players[i];
        QString role = roles.at(i);

        player->setRole(role);
        if ((role == "lord" && !ServerInfo.EnableHegemony)
            || mode == "04_1v3" || mode == "04_boss" || mode == "08_defense")
            broadcastProperty(player, "role", player->getRole());
        else
            notifyProperty(player, player, "role");
    }
}

void Room::swapSeat(ServerPlayer *a, ServerPlayer *b)
{
    int seat1 = m_players.indexOf(a);
    int seat2 = m_players.indexOf(b);

    m_players.swap(seat1, seat2);

    QStringList player_circle;
    foreach(ServerPlayer *player, m_players)
        player_circle << player->objectName();
    doBroadcastNotify(S_COMMAND_ARRANGE_SEATS, JsonUtils::toJsonArray(player_circle));

    m_alivePlayers.clear();
    for (int i = 0; i < m_players.length(); i++) {
        ServerPlayer *player = m_players.at(i);
        if (player->isAlive()) {
            m_alivePlayers << player;
            player->setSeat(m_alivePlayers.length());
        } else {
            player->setSeat(0);
        }

        broadcastProperty(player, "seat");

        player->setNext(m_players.at((i + 1) % m_players.length()));
    }
}

void Room::adjustSeats()
{
    QList<ServerPlayer *> players;
    int i = 0;
    for (i = 0; i < m_players.length(); i++) {
        if (m_players.at(i)->getRoleEnum() == Player::Lord)
            break;
    }
    for (int j = i; j < m_players.length(); j++)
        players << m_players.at(j);
    for (int j = 0; j < i; j++)
        players << m_players.at(j);

    m_players = players;

    for (int i = 0; i < m_players.length(); i++)
        m_players.at(i)->setSeat(i + 1);

    // tell the players about the seat, and the first is always the lord
    QStringList player_circle;
    foreach(ServerPlayer *player, m_players)
        player_circle << player->objectName();
    doBroadcastNotify(S_COMMAND_ARRANGE_SEATS, JsonUtils::toJsonArray(player_circle));
}

int Room::getCardFromPile(const QString &card_pattern)
{
    if (m_drawPile->isEmpty())
        swapPile();

    if (card_pattern.startsWith("@")) {
        if (card_pattern == "@duanliang") {
            foreach (int card_id, *m_drawPile) {
                const Card *card = Sanguosha->getCard(card_id);
                if (card->isBlack() && (card->isKindOf("BasicCard") || card->isKindOf("EquipCard")))
                    return card_id;
            }
        }
    } else {
        QString card_name = card_pattern;
        foreach (int card_id, *m_drawPile) {
            const Card *card = Sanguosha->getCard(card_id);
            if (card->objectName() == card_name)
                return card_id;
        }
    }

    return -1;
}

QString Room::_chooseDefaultGeneral(ServerPlayer *player) const
{
    Q_ASSERT(!player->getSelected().isEmpty());
    if (Config.EnableHegemony && Config.Enable2ndGeneral) {
        foreach (QString name, player->getSelected()) {
            Q_ASSERT(!name.isEmpty());
            if (player->getGeneral() != NULL) { // choosing first general
                if (name == player->getGeneralName()) continue;
                if (Sanguosha->getGeneral(name)->getKingdom() == player->getGeneral()->getKingdom())
                    return name;
            } else {
                foreach (QString other, player->getSelected()) { // choosing second general
                    if (name == other) continue;
                    if (Sanguosha->getGeneral(name)->getKingdom() == Sanguosha->getGeneral(other)->getKingdom())
                        return name;
                }
            }
        }
        Q_ASSERT(false);
        return QString();
    } else {
        GeneralSelector *selector = GeneralSelector::getInstance();
        QString choice = selector->selectFirst(player, player->getSelected());
        return choice;
    }
}

bool Room::_setPlayerGeneral(ServerPlayer *player, const QString &generalName, bool isFirst)
{
    const General *general = Sanguosha->getGeneral(generalName);
    if (general == NULL)
        return false;
    else if (!Config.FreeChoose && !player->getSelected().contains(generalName))
        return false;

    if (isFirst) {
        player->setGeneralName(general->objectName());
        notifyProperty(player, player, "general");
    } else {
        player->setGeneral2Name(general->objectName());
        notifyProperty(player, player, "general2");
    }
    return true;
}

void Room::speakCommand(ServerPlayer *player, const QString &arg)
{
    return speakCommand(player, QVariant(arg));
}

void Room::speakCommand(ServerPlayer *player, const QVariant &arg)
{
#define _NO_BROADCAST_SPEAKING {\
                                   broadcast = false;\
                                   JsonArray nbbody;\
                                   nbbody << player->objectName();\
                                   nbbody << arg;\
                                   doNotify(player, S_COMMAND_SPEAK, nbbody);\
                               }
    bool broadcast = true;
    if (player && Config.EnableCheat) {
        QString sentence = QString::fromUtf8(QByteArray::fromBase64(arg.toString().toLatin1()));
        if (sentence == ".BroadcastRoles") {
            _NO_BROADCAST_SPEAKING
                foreach(ServerPlayer *p, m_alivePlayers)
                broadcastProperty(p, "role", p->getRole());
        } else if (sentence.startsWith(".BroadcastRoles=")) {
            _NO_BROADCAST_SPEAKING
                QString name = sentence.mid(12);
            foreach (ServerPlayer *p, m_alivePlayers) {
                if (p->objectName() == name || p->getGeneralName() == name) {
                    broadcastProperty(p, "role", p->getRole());
                    break;
                }
            }
        } else if (sentence == ".ShowHandCards") {
            _NO_BROADCAST_SPEAKING
                QString split("----------");
            split = split.toUtf8().toBase64();

            JsonArray body;
            body << player->objectName() << split;
            doNotify(player, S_COMMAND_SPEAK, body);

            foreach (ServerPlayer *p, m_alivePlayers) {
                if (!p->isKongcheng()) {
                    QStringList handcards;
                    foreach (const Card *card, p->getHandcards())
                        handcards << QString("<b>%1</b>")
                        .arg(Sanguosha->getEngineCard(card->getId())->getLogName());
                    QString hand = handcards.join(", ");
                    hand = hand.toUtf8().toBase64();

                    JsonArray body;
                    body << p->objectName() << hand;
                    doNotify(player, S_COMMAND_SPEAK, body);
                }
            }
            doNotify(player, S_COMMAND_SPEAK, body);
        } else if (sentence.startsWith(".ShowHandCards=")) {
            _NO_BROADCAST_SPEAKING
                QString name = sentence.mid(15);
            foreach (ServerPlayer *p, m_alivePlayers) {
                if (p->objectName() == name || p->getGeneralName() == name) {
                    if (!p->isKongcheng()) {
                        QStringList handcards;
                        foreach(const Card *card, p->getHandcards())
                            handcards << QString("<b>%1</b>")
                            .arg(Sanguosha->getEngineCard(card->getId())->getLogName());
                        QString hand = handcards.join(", ");
                        hand = hand.toUtf8().toBase64();

                        JsonArray body;
                        body << p->objectName() << hand;
                        doNotify(player, S_COMMAND_SPEAK, body);
                    }
                    break;
                }
            }
        } else if (sentence.startsWith(".ShowPrivatePile=")) {
            _NO_BROADCAST_SPEAKING
                QStringList arg = sentence.mid(17).split(":");
            if (arg.length() == 2) {
                QString name = arg.first();
                QString pile_name = arg.last();
                foreach (ServerPlayer *p, m_alivePlayers) {
                    if (p->objectName() == name || p->getGeneralName() == name) {
                        if (!p->getPile(pile_name).isEmpty()) {
                            QStringList pile_cards;
                            foreach(int id, p->getPile(pile_name))
                                pile_cards << QString("<b>%1</b>").arg(Sanguosha->getEngineCard(id)->getLogName());
                            QString pile = pile_cards.join(", ");
                            pile = pile.toUtf8().toBase64();

                            JsonArray body;
                            body << p->objectName() << pile;
                            doNotify(player, S_COMMAND_SPEAK, body);
                        }
                        break;
                    }
                }
            }
        } else if (sentence == ".ShowHuashen") {
            _NO_BROADCAST_SPEAKING
                QList<ServerPlayer *> zuocis = findPlayersBySkillName("huashen");
            QStringList huashen_name;
            foreach (ServerPlayer *zuoci, zuocis) {
                QVariantList huashens = zuoci->tag["Huashens"].toList();
                huashen_name.clear();
                foreach(QVariant name, huashens)
                    huashen_name << QString("<b>%1</b>").arg(Sanguosha->translate(name.toString()));
                QString huashen = huashen_name.join(", ");
                huashen = huashen.toUtf8().toBase64();

                JsonArray body;
                body << zuoci->objectName() << huashen;
                doNotify(player, S_COMMAND_SPEAK, body);
            }
        } else if (sentence.startsWith(".SetAIDelay=")) {
            _NO_BROADCAST_SPEAKING
                bool ok = false;
            int delay = sentence.mid(12).toInt(&ok);
            if (ok) {
                Config.AIDelay = Config.OriginAIDelay = delay;
                Config.setValue("OriginAIDelay", delay);
            }
        } else if (sentence.startsWith(".SetGameMode=")) {
            _NO_BROADCAST_SPEAKING
                QString name = sentence.mid(13);
            setTag("NextGameMode", name);
        } else if (sentence.startsWith(".SecondGeneral=")) {
            _NO_BROADCAST_SPEAKING
                QString prop = sentence.mid(15);
            setTag("NextGameSecondGeneral", !prop.isEmpty() && prop != "0" && prop != "false");
        } else if (sentence == ".Pause") {
            _NO_BROADCAST_SPEAKING
                pauseCommand(player, true);
        } else if (sentence == ".Resume") {
            _NO_BROADCAST_SPEAKING
                pauseCommand(player, false);
        }
    }
    if (broadcast && player != NULL) {
        JsonArray body;
        body << player->objectName() << arg;
        doBroadcastNotify(S_COMMAND_SPEAK, body);
    }
    return;
#undef _NO_BROADCAST_SPEAKING
}

void Room::processResponse(ServerPlayer *player, const Packet *packet)
{
    player->acquireLock(ServerPlayer::SEMA_MUTEX);
    bool success = false;
    if (player == NULL)
        emit room_message(tr("Unable to parse player"));
    else if (!player->m_isWaitingReply || player->m_isClientResponseReady)
        emit room_message(tr("Server is not waiting for reply from %1").arg(player->objectName()));
    else if (packet->getCommandType() != player->m_expectedReplyCommand)
        emit room_message(tr("Reply command should be %1 instead of %2")
        .arg(player->m_expectedReplyCommand).arg(packet->getCommandType()));
    else if (packet->localSerial != player->m_expectedReplySerial)
        emit room_message(tr("Reply serial should be %1 instead of %2")
        .arg(player->m_expectedReplySerial).arg(packet->localSerial));
    else
        success = true;

    if (!success) {
        player->releaseLock(ServerPlayer::SEMA_MUTEX);
        return;
    } else {
        _m_semRoomMutex.acquire();
        if (_m_raceStarted) {
            player->setClientReply(packet->getMessageBody());
            player->m_isClientResponseReady = true;
            // Warning: the statement below must be the last one before releasing the lock!!!
            // Any statement after this statement will totally compromise the synchronization
            // because getRaceResult will then be able to acquire the lock, reading a non-null
            // raceWinner and proceed with partial data. The current implementation is based on
            // the assumption that the following line is ATOMIC!!!
            // @todo: Find a Qt atomic semantic or use _asm to ensure the following line is atomic
            // on a multi-core machine. This is the core to the whole synchornization mechanism for
            // broadcastRaceRequest.
            _m_raceWinner = player;
            // the _m_semRoomMutex.release() signal is in getRaceResult();
            _m_semRaceRequest.release();
        } else {
            _m_semRoomMutex.release();
            player->setClientReply(packet->getMessageBody());
            player->m_isClientResponseReady = true;
            player->releaseLock(ServerPlayer::SEMA_COMMAND_INTERACTIVE);
        }

        player->releaseLock(ServerPlayer::SEMA_MUTEX);
    }
}

bool Room::useCard(const CardUseStruct &use, bool add_history)
{
    CardUseStruct card_use = use;
    card_use.m_addHistory = false;
    card_use.m_isHandcard = true;
    const Card *card = card_use.card;
    QList<int> ids;
    if (!card->isVirtualCard()) ids << card->getEffectiveId();
    else ids = card->getSubcards();
    if (!ids.isEmpty()) {
        foreach (int id, ids) {
            if (getCardOwner(id) != use.from || getCardPlace(id) != Player::PlaceHand) {
                card_use.m_isHandcard = false;
                break;
            }
        }
    } else {
        card_use.m_isHandcard = false;
    }

    if (card_use.from->isCardLimited(card, card->getHandlingMethod())
        && (!card->canRecast() || card_use.from->isCardLimited(card, Card::MethodRecast)))
        return true;

    QString key;
    if (card->inherits("LuaSkillCard"))
        key = "#" + card->objectName();
    else
        key = card->getClassName();
    int slash_count = card_use.from->getSlashCount();
    bool slash_not_record = key.contains("Slash")
        && slash_count > 0
        && (card_use.from->hasWeapon("crossbow")
        || Sanguosha->correctCardTarget(TargetModSkill::Residue, card_use.from, card) > 500);

    card = card_use.card->validate(card_use);
    if (card == NULL)
        return false;

    if (card_use.from->getPhase() == Player::Play && add_history) {
        if (!slash_not_record) {
            card_use.m_addHistory = true;
            addPlayerHistory(card_use.from, key);
        }
        addPlayerHistory(NULL, "pushPile");
    }

    try {
        if (card_use.card->getRealCard() == card) {
            if (card->isKindOf("DelayedTrick") && card->isVirtualCard() && card->subcardsLength() == 1) {
                Card *trick = Sanguosha->cloneCard(card);
                Q_ASSERT(trick != NULL);
                WrappedCard *wrapped = Sanguosha->getWrappedCard(card->getSubcards().first());
                wrapped->takeOver(trick);
                broadcastUpdateCard(getPlayers(), wrapped->getId(), wrapped);
                card_use.card = wrapped;
                wrapped->onUse(this, card_use);
                return true;
            }
            if (card_use.card->isKindOf("Slash") && add_history && slash_count > 0)
                card_use.from->setFlags("Global_MoreSlashInOneTurn");
            if (!card_use.card->isVirtualCard()) {
                WrappedCard *wrapped = Sanguosha->getWrappedCard(card_use.card->getEffectiveId());
                if (wrapped->isModified())
                    broadcastUpdateCard(getPlayers(), card_use.card->getEffectiveId(), wrapped);
                else
                    broadcastResetCard(getPlayers(), card_use.card->getEffectiveId());
            }
            card_use.card->onUse(this, card_use);
        } else if (card) {
            CardUseStruct new_use = card_use;
            new_use.card = card;
            useCard(new_use, add_history);
            if (card->isVirtualCard() && !card->isKindOf("Nullification")) // delete Nullification in askForNullification
                delete card;
        }
    }
    catch (TriggerEvent triggerEvent) {
        if (triggerEvent == StageChange || triggerEvent == TurnBroken) {
            QList<int> card_ids;
            if (card_use.card->isVirtualCard())
                card_ids = card_use.card->getSubcards();
            else
                card_ids << card_use.card->getId();
            QList<CardsMoveStruct> moves;
            CardMoveReason reason(CardMoveReason::S_REASON_UNKNOWN, card_use.from->objectName(), QString(), card_use.card->getSkillName(), QString());
            if (card_use.to.size() == 1) reason.m_targetId = card_use.to.first()->objectName();
            foreach (int id, card_ids) {
                if (getCardPlace(id) == Player::PlaceTable) {
                    CardsMoveStruct move(id, card_use.from, NULL, Player::PlaceTable, Player::DiscardPile, reason);
                    moves << move;
                }
            }
            if (!moves.isEmpty())
                moveCardsAtomic(moves, true);
            QVariant data = QVariant::fromValue(card_use);
            card_use.from->setFlags("Global_ProcessBroken");
            thread->trigger(CardFinished, this, card_use.from, data);
            card_use.from->setFlags("-Global_ProcessBroken");

            foreach (ServerPlayer *p, m_alivePlayers) {
                p->tag.remove("Qinggang");

                foreach (QString flag, p->getFlagList()) {
                    if (flag == "Global_GongxinOperator")
                        p->setFlags("-" + flag);
                    else if (flag.endsWith("_InTempMoving"))
                        setPlayerFlag(p, "-" + flag);
                }
            }

            foreach (int id, Sanguosha->getRandomCards()) {
                if (getCardPlace(id) == Player::PlaceTable || getCardPlace(id) == Player::PlaceJudge)
                    moveCardTo(Sanguosha->getCard(id), NULL, Player::DiscardPile, true);
                if (Sanguosha->getCard(id)->hasFlag("using"))
                    setCardFlag(id, "-using");
            }
        }
        throw triggerEvent;
    }
    return true;
}

void Room::loseHp(ServerPlayer *victim, int lose)
{
    if (victim->isDead())
        return;
    QVariant data = lose;
    if (thread->trigger(PreHpLost, this, victim, data))
        return;

    LogMessage log;
    log.type = "#LoseHp";
    log.from = victim;
    log.arg = QString::number(lose);
    sendLog(log);

    JsonArray arg;
    arg << victim->objectName();
    arg << -lose;
    arg << -1;
    doBroadcastNotify(S_COMMAND_CHANGE_HP, arg);

    setTag("HpChangedData", data);
    setPlayerProperty(victim, "hp", victim->getHp() - lose);

    thread->trigger(HpLost, this, victim, data);
}

void Room::loseMaxHp(ServerPlayer *victim, int lose)
{
    victim->setMaxHp(qMax(victim->getMaxHp() - lose, 0));

    broadcastProperty(victim, "maxhp");
    broadcastProperty(victim, "hp");

    LogMessage log;
    log.type = "#LoseMaxHp";
    log.from = victim;
    log.arg = QString::number(lose);
    sendLog(log);

    JsonArray arg;
    arg << victim->objectName();
    arg << -lose;
    doBroadcastNotify(S_COMMAND_CHANGE_MAXHP, arg);

    if (victim->getMaxHp() == 0)
        killPlayer(victim);
    else
        thread->trigger(MaxHpChanged, this, victim);
}

bool Room::changeMaxHpForAwakenSkill(ServerPlayer *player, int magnitude)
{
    addPlayerMark(player, "@waked");
    int n = player->getMark("@waked");
    if (magnitude < 0) {
        if (Config.Enable2ndGeneral && player->getGeneral() && player->getGeneral2()
            && Config.MaxHpScheme > 0 && Config.PreventAwakenBelow3
            && player->getMaxHp() <= 3) {
            setPlayerMark(player, "AwakenLostMaxHp", 1);
        } else {
            loseMaxHp(player, -magnitude);
        }
    } else if (magnitude > 0) {
        LogMessage log;
        log.type = "#GainMaxHp";
        log.from = player;
        log.arg = QString::number(magnitude);
        sendLog(log);

        setPlayerProperty(player, "maxhp", player->getMaxHp() + magnitude);

        LogMessage log2;
        log2.type = "#GetHp";
        log2.from = player;
        log2.arg = QString::number(player->getHp());
        log2.arg2 = QString::number(player->getMaxHp());
        sendLog(log2);
    }
    return (player->getMark("@waked") >= n);
}

void Room::recover(ServerPlayer *player, const RecoverStruct &recover, bool set_emotion)
{
    if (player->getLostHp() == 0 || player->isDead())
        return;
    RecoverStruct recover_struct = recover;

    QVariant data = QVariant::fromValue(recover_struct);
    if (thread->trigger(PreHpRecover, this, player, data))
        return;

    recover_struct = data.value<RecoverStruct>();
    recover_struct.recover = qMin(player->getMaxHp() - player->getHp(), recover_struct.recover);
    int recover_num = recover_struct.recover;

    JsonArray arg;
    arg << player->objectName();
    arg << recover_num;
    arg << 0;
    doBroadcastNotify(S_COMMAND_CHANGE_HP, arg);

    int new_hp = qMin(player->getHp() + recover_num, player->getMaxHp());
    setTag("HpChangedData", data);
    setPlayerProperty(player, "hp", new_hp);

    if (set_emotion)
        setEmotion(player, "recover");

    thread->trigger(HpRecover, this, player, data);
}

bool Room::cardEffect(const Card *card, ServerPlayer *from, ServerPlayer *to, bool multiple)
{
    CardEffectStruct effect;
    effect.card = card;
    effect.from = from;
    effect.to = to;
    effect.multiple = multiple;

    return cardEffect(effect);
}

bool Room::cardEffect(const CardEffectStruct &effect)
{
    QVariant data = QVariant::fromValue(effect);
    bool cancel = false;
    if (effect.to->isAlive() || effect.card->isKindOf("Slash")) { // Be care!!!
        // No skills should be triggered here!
        thread->trigger(CardEffect, this, effect.to, data);
        // Make sure that effectiveness of Slash isn't judged here!
        if (!thread->trigger(CardEffected, this, effect.to, data)) {
            cancel = true;
        } else {
            if (!effect.to->hasFlag("Global_NonSkillNullify"))
                setEmotion(effect.to, "skill_nullify");
            else
                effect.to->setFlags("-Global_NonSkillNullify");
        }
    }
    thread->trigger(PostCardEffected, this, effect.to, data);
    return cancel;
}

bool Room::isJinkEffected(ServerPlayer *user, const Card *jink)
{
    if (jink == NULL || user == NULL)
        return false;
    Q_ASSERT(jink->isKindOf("Jink"));
    QVariant jink_data = QVariant::fromValue(jink);
    return !thread->trigger(JinkEffect, this, user, jink_data);
}

void Room::damage(const DamageStruct &data)
{
    DamageStruct damage_data = data;
    if (damage_data.to == NULL || damage_data.to->isDead())
        return;

    QVariant qdata = QVariant::fromValue(damage_data);

    if (!damage_data.chain && !damage_data.transfer) {
        thread->trigger(ConfirmDamage, this, damage_data.from, qdata);
        damage_data = qdata.value<DamageStruct>();
    }

#define REMOVE_QINGGANG_TAG \
    do { \
        if (damage_data.card && damage_data.card->isKindOf("Slash")) \
            damage_data.to->removeQinggangTag(damage_data.card); \
    } while (false) // ;

    // Predamage
    if (thread->trigger(Predamage, this, damage_data.from, qdata)) {
        REMOVE_QINGGANG_TAG;
        return;
    }

    try {
        bool enter_stack = false;
        do {
            if (thread->trigger(DamageForseen, this, damage_data.to, qdata)) {
                REMOVE_QINGGANG_TAG;
                break;
            }

            if (damage_data.from) {
                if (thread->trigger(DamageCaused, this, damage_data.from, qdata)) {
                    REMOVE_QINGGANG_TAG;
                    break;
                }
            }

            damage_data = qdata.value<DamageStruct>();
            damage_data.to->tag.remove("TransferDamage");
            if (thread->trigger(DamageInflicted, this, damage_data.to, qdata)) {
                REMOVE_QINGGANG_TAG;
                // Make sure that the trigger in which 'TransferDamage' tag is set returns TRUE
                DamageStruct transfer_damage_data = damage_data.to->tag["TransferDamage"].value<DamageStruct>();
                if (transfer_damage_data.to)
                    damage(transfer_damage_data);
                break;
            }

            enter_stack = true;
            m_damageStack.push_back(damage_data);
            setTag("CurrentDamageStruct", qdata);

            thread->trigger(PreDamageDone, this, damage_data.to, qdata);
            
            if (damage_data.from != NULL) {
                int d = qdata.value<DamageStruct>().damage;
                int f = damage_data.from->getMark("damage_point_round");
                setPlayerMark(damage_data.from, "damage_point_round", d + f);
                if (damage_data.from->getPhase() == Player::Play) {
                    f = damage_data.from->getMark("damage_point_play_phase");
                    setPlayerMark(damage_data.from, "damage_point_play_phase", d + f);
                }
            }

            REMOVE_QINGGANG_TAG;
            thread->trigger(DamageDone, this, damage_data.to, qdata);

            if (damage_data.from && !damage_data.from->hasFlag("Global_DebutFlag"))
                thread->trigger(Damage, this, damage_data.from, qdata);

            if (!damage_data.to->hasFlag("Global_DebutFlag"))
                thread->trigger(Damaged, this, damage_data.to, qdata);
        } while (false);
#undef REMOVE_QINGGANG_TAG

        damage_data = qdata.value<DamageStruct>();
        if (!enter_stack) {
            damage_data.prevented = true;
            qdata = QVariant::fromValue(damage_data);
        }
        thread->trigger(DamageComplete, this, damage_data.to, qdata);

        if (enter_stack) {
            m_damageStack.pop();
            if (m_damageStack.isEmpty())
                removeTag("CurrentDamageStruct");
            else
                setTag("CurrentDamageStruct", QVariant::fromValue(m_damageStack.first()));
        }
    }
    catch (TriggerEvent triggerEvent) {
        if (triggerEvent == StageChange || triggerEvent == TurnBroken) {
            removeTag("is_chained");
            removeTag("CurrentDamageStruct");
            m_damageStack.clear();
        }
        throw triggerEvent;
    }
}

bool Room::hasWelfare(const ServerPlayer *player) const
{
    if (mode == "06_3v3")
        return player->isLord() || player->getRole() == "renegade";
    else if (Config.EnableHegemony || mode == "06_XMode")
        return false;
    else
        return player->isLord() && player_count > 4;
}

ServerPlayer *Room::getFront(ServerPlayer *a, ServerPlayer *b) const
{
    QList<ServerPlayer *> players = getAllPlayers(true);
    int index_a = players.indexOf(a), index_b = players.indexOf(b);
    if (index_a < index_b)
        return a;
    else
        return b;
}

void Room::reconnect(ServerPlayer *player, ClientSocket *socket)
{
    player->setSocket(socket);
    player->setState("online");

    marshal(player);

    broadcastProperty(player, "state");
}

void Room::marshal(ServerPlayer *player)
{
    notifyProperty(player, player, "objectName");
    notifyProperty(player, player, "role");
    notifyProperty(player, player, "flags", "marshalling");

    foreach (ServerPlayer *p, m_players) {
        if (p != player)
            p->introduceTo(player);
    }

    QStringList player_circle;
    foreach(ServerPlayer *player, m_players)
        player_circle << player->objectName();
    doNotify(player, S_COMMAND_ARRANGE_SEATS, JsonUtils::toJsonArray(player_circle));

    doNotify(player, S_COMMAND_START_IN_X_SECONDS, QVariant(0));

    foreach (ServerPlayer *p, m_players) {
        notifyProperty(player, p, "general");

        if (p->getGeneral2())
            notifyProperty(player, p, "general2");

        notifyProperty(player, p, "state");
    }

    if (game_started) {
        doNotify(player, S_COMMAND_GAME_START, QVariant());

        QList<int> drawPile = Sanguosha->getRandomCards();
        doNotify(player, S_COMMAND_AVAILABLE_CARDS, JsonUtils::toJsonArray(drawPile));
    }

    foreach(ServerPlayer *p, m_players)
        p->marshal(player);

    notifyProperty(player, player, "flags", "-marshalling");

    if (game_started) {
        doNotify(player, S_COMMAND_UPDATE_PILE, QVariant(m_drawPile->length()));

        if (!m_fillAGarg.isNull()) {
            doNotify(player, S_COMMAND_FILL_AMAZING_GRACE, m_fillAGarg);
            foreach(const QVariant &takeAGarg, m_takeAGargs.value<JsonArray>())
                doNotify(player, S_COMMAND_TAKE_AMAZING_GRACE, takeAGarg);
        }

        QVariant discard = JsonUtils::toJsonArray(*m_discardPile);
        doNotify(player, S_COMMAND_SYNCHRONIZE_DISCARD_PILE, discard);
    }
}

void Room::startGame()
{
    m_alivePlayers = m_players;
    if (mode != "08_defense") {
        for (int i = 0; i < player_count - 1; i++)
            m_players.at(i)->setNext(m_players.at(i + 1));
        m_players.last()->setNext(m_players.first());
    } else {
        QList<int> next_list;
        next_list << 0 << 7 << 1 << 6 << 2 << 5 << 3 << 4;
        for (int i = 0; i < player_count - 1; i++)
            m_players.at(next_list.at(i))->setNext(m_players.at(next_list.at(i + 1)));
        m_players.at(4)->setNext(m_players.first());
    }

    foreach (ServerPlayer *player, m_players) {
        Q_ASSERT(player->getGeneral());
        player->setMaxHp(player->getGeneralMaxHp());
        player->setHp(player->getMaxHp());
        // setup AI
        AI *ai = cloneAI(player);
        ais << ai;
        player->setAI(ai);
    }

    foreach (ServerPlayer *player, m_players) {
        if (!Config.EnableBasara
            && (mode == "06_3v3" || mode == "02_1v1" || mode == "06_XMode" || !player->isLord()))
            broadcastProperty(player, "general");

        if (mode == "02_1v1")
            doBroadcastNotify(getOtherPlayers(player, true), S_COMMAND_REVEAL_GENERAL, JsonArray() << player->objectName() << player->getGeneralName());

        if (Config.Enable2ndGeneral
            && mode != "02_1v1" && mode != "06_3v3" && mode != "06_XMode" && mode != "04_1v3"
            && !Config.EnableBasara)
            broadcastProperty(player, "general2");

        broadcastProperty(player, "hp");
        broadcastProperty(player, "maxhp");

        if (mode == "06_3v3" || mode == "06_XMode")
            broadcastProperty(player, "role");
    }

    preparePlayers();

    QList<int> drawPile = *m_drawPile;
    qShuffle(drawPile);
    doBroadcastNotify(S_COMMAND_AVAILABLE_CARDS, JsonUtils::toJsonArray(drawPile));

    doBroadcastNotify(S_COMMAND_GAME_START, QVariant());
    game_started = true;

    Server *server = qobject_cast<Server *>(parent());
    foreach (ServerPlayer *player, m_players) {
        if (player->getState() == "online")
            server->signupPlayer(player);
    }

    current = m_players.first();

    // initialize the place_map and owner_map;
    foreach(int card_id, *m_drawPile)
        setCardMapping(card_id, NULL, Player::DrawPile);
    doBroadcastNotify(S_COMMAND_UPDATE_PILE, QVariant(m_drawPile->length()));

    thread = new RoomThread(this);
    if (mode != "02_1v1" && mode != "06_3v3" && mode != "06_XMode")
        _m_roomState.reset();
    //connect(thread, SIGNAL(started()), this, SIGNAL(game_start()));

    if (!_virtual) thread->start();
}

bool Room::notifyProperty(ServerPlayer *playerToNotify, const ServerPlayer *propertyOwner, const char *propertyName, const QString &value)
{
    if (propertyOwner == NULL) return false;
    QString real_value = value;
    if (real_value.isNull()) real_value = propertyOwner->property(propertyName).toString();
    JsonArray arg;
    if (propertyOwner == playerToNotify)
        arg << QSanProtocol::S_PLAYER_SELF_REFERENCE_ID;
    else
        arg << propertyOwner->objectName();
    arg << propertyName;
    arg << real_value;
    return doNotify(playerToNotify, S_COMMAND_SET_PROPERTY, arg);
}

bool Room::broadcastProperty(ServerPlayer *player, const char *property_name, const QString &value)
{
    if (player == NULL) return false;
    QString real_value = value;
    if (real_value.isNull()) real_value = player->property(property_name).toString();

    if (strcmp(property_name, "role") == 0)
        player->setShownRole(true);

    JsonArray arg;
    arg << player->objectName() << property_name << real_value;
    return doBroadcastNotify(S_COMMAND_SET_PROPERTY, arg);
}

void Room::drawCards(ServerPlayer *player, int n, const QString &reason)
{
    QList<ServerPlayer *> players;
    players.append(player);
    drawCards(players, n, reason);
}

void Room::drawCards(QList<ServerPlayer *> players, int n, const QString &reason)
{
    QList<int> n_list;
    n_list.append(n);
    drawCards(players, n_list, reason);
}

void Room::drawCards(QList<ServerPlayer *> players, QList<int> n_list, const QString &reason)
{
    QList<CardsMoveStruct> moves;
    int index = -1, len = n_list.length();
    Q_ASSERT(len >= 1);
    foreach (ServerPlayer *player, players) {
        index++;
        if (!player->isAlive() && reason != "reform") continue;
        int n = n_list.at(qMin(index, len - 1));
        if (n <= 0) continue;
        QList<int> card_ids = getNCards(n, false);

        CardsMoveStruct move;
        move.card_ids = card_ids;
        move.from = NULL;
        move.to = player;
        move.to_place = Player::PlaceHand;
        move.reason = CardMoveReason(CardMoveReason::S_REASON_DRAW, player->objectName(), reason, QString());
        moves.append(move);
    }
    moveCardsAtomic(moves, false);
}

void Room::throwCard(const Card *card, ServerPlayer *who, ServerPlayer *thrower)
{
    CardMoveReason reason;
    if (thrower == NULL) {
        reason.m_reason = CardMoveReason::S_REASON_THROW;
        reason.m_playerId = who ? who->objectName() : QString();
    } else {
        reason.m_reason = CardMoveReason::S_REASON_DISMANTLE;
        reason.m_targetId = who ? who->objectName() : QString();
        reason.m_playerId = thrower->objectName();
    }
    reason.m_skillName = card->getSkillName();
    throwCard(card, reason, who, thrower);
}

void Room::throwCard(const Card *card, const CardMoveReason &reason, ServerPlayer *who, ServerPlayer *thrower)
{
    if (card == NULL)
        return;

    QList<int> to_discard;
    if (card->isVirtualCard())
        to_discard.append(card->getSubcards());
    else
        to_discard << card->getEffectiveId();

    LogMessage log;
    if (who) {
        if (thrower == NULL) {
            log.type = "$DiscardCard";
            log.from = who;
        } else {
            log.type = "$DiscardCardByOther";
            log.from = thrower;
            log.to << who;
        }
    } else {
        log.type = "$EnterDiscardPile";
    }
    log.card_str = IntList2StringList(to_discard).join("+");
    sendLog(log);

    QList<CardsMoveStruct> moves;
    if (who) {
        CardsMoveStruct move(to_discard, who, NULL, Player::PlaceUnknown, Player::DiscardPile, reason);
        moves.append(move);
        moveCardsAtomic(moves, true);
    } else {
        CardsMoveStruct move(to_discard, NULL, Player::DiscardPile, reason);
        moves.append(move);
        moveCardsAtomic(moves, true);
    }
}

void Room::throwCard(int card_id, ServerPlayer *who, ServerPlayer *thrower)
{
    throwCard(Sanguosha->getCard(card_id), who, thrower);
}

RoomThread *Room::getThread() const
{
    return thread;
}

void Room::moveCardTo(const Card *card, ServerPlayer *dstPlayer, Player::Place dstPlace, bool forceMoveVisible)
{
    moveCardTo(card, dstPlayer, dstPlace,
        CardMoveReason(CardMoveReason::S_REASON_UNKNOWN, QString()), forceMoveVisible);
}

void Room::moveCardTo(const Card *card, ServerPlayer *dstPlayer, Player::Place dstPlace,
    const CardMoveReason &reason, bool forceMoveVisible)
{
    moveCardTo(card, NULL, dstPlayer, dstPlace, QString(), reason, forceMoveVisible);
}

void Room::moveCardTo(const Card *card, ServerPlayer *srcPlayer, ServerPlayer *dstPlayer, Player::Place dstPlace,
    const CardMoveReason &reason, bool forceMoveVisible)
{
    moveCardTo(card, srcPlayer, dstPlayer, dstPlace, QString(), reason, forceMoveVisible);
}

void Room::moveCardTo(const Card *card, ServerPlayer *srcPlayer, ServerPlayer *dstPlayer, Player::Place dstPlace,
    const QString &pileName, const CardMoveReason &reason, bool forceMoveVisible)
{
    CardsMoveStruct move;
    if (card->isVirtualCard()) {
        move.card_ids = card->getSubcards();
        if (move.card_ids.size() == 0) return;
    } else
        move.card_ids.append(card->getId());
    move.to = dstPlayer;
    move.to_place = dstPlace;
    move.to_pile_name = pileName;
    move.from = srcPlayer;
    move.reason = reason;
    QList<CardsMoveStruct> moves;
    moves.append(move);
    moveCardsAtomic(moves, forceMoveVisible);
}

void Room::_fillMoveInfo(CardsMoveStruct &moves, int card_index) const
{
    int card_id = moves.card_ids[card_index];
    if (!moves.from)
        moves.from = getCardOwner(card_id);
    moves.from_place = getCardPlace(card_id);
    if (moves.from) { // Hand/Equip/Judge
        if (moves.from_place == Player::PlaceSpecial || moves.from_place == Player::PlaceTable)
            moves.from_pile_name = moves.from->getPileName(card_id);
        if (moves.from_player_name.isEmpty())
            moves.from_player_name = moves.from->objectName();
    }
    if (moves.to) {
        if (moves.to_player_name.isEmpty())
            moves.to_player_name = moves.to->objectName();
        int card_id = moves.card_ids[card_index];
        if (moves.to_place == Player::PlaceSpecial || moves.to_place == Player::PlaceTable)
            moves.to_pile_name = moves.to->getPileName(card_id);
    }
}

static bool CompareByActionOrder_OneTime(CardsMoveOneTimeStruct move1, CardsMoveOneTimeStruct move2)
{
    ServerPlayer *a = (ServerPlayer *)move1.from;
    if (a == NULL) a = (ServerPlayer *)move1.to;
    ServerPlayer *b = (ServerPlayer *)move2.from;
    if (b == NULL) b = (ServerPlayer *)move2.to;

    if (a == NULL || b == NULL)
        return a != NULL;

    Room *room = a->getRoom();
    return room->getFront(a, b) == a;
}

static bool CompareByActionOrder(CardsMoveStruct move1, CardsMoveStruct move2)
{
    ServerPlayer *a = (ServerPlayer *)move1.from;
    if (a == NULL) a = (ServerPlayer *)move1.to;
    ServerPlayer *b = (ServerPlayer *)move2.from;
    if (b == NULL) b = (ServerPlayer *)move2.to;

    if (a == NULL || b == NULL)
        return a != NULL;

    Room *room = a->getRoom();
    return room->getFront(a, b) == a;
}

QList<CardsMoveOneTimeStruct> Room::_mergeMoves(QList<CardsMoveStruct> cards_moves)
{
    QMap<_MoveMergeClassifier, QList<CardsMoveStruct> > moveMap;

    foreach (CardsMoveStruct cards_move, cards_moves) {
        _MoveMergeClassifier classifier(cards_move);
        moveMap[classifier].append(cards_move);
    }

    QList<CardsMoveOneTimeStruct> result;
    foreach (_MoveMergeClassifier cls, moveMap.keys()) {
        CardsMoveOneTimeStruct moveOneTime;
        moveOneTime.from = cls.m_from;
        moveOneTime.reason = moveMap[cls].first().reason;
        moveOneTime.to = cls.m_to;
        moveOneTime.to_place = cls.m_to_place;
        moveOneTime.to_pile_name = cls.m_to_pile_name;
        moveOneTime.is_last_handcard = false;
        foreach (CardsMoveStruct move, moveMap[cls]) {
            moveOneTime.card_ids.append(move.card_ids);
            for (int i = 0; i < move.card_ids.size(); i++) {
                moveOneTime.from_places.append(move.from_place);
                moveOneTime.from_pile_names.append(move.from_pile_name);
                moveOneTime.open.append(move.open);
            }
            if (move.is_last_handcard)
                moveOneTime.is_last_handcard = true;
        }
        result.append(moveOneTime);
    }

    if (result.size() > 1)
        qSort(result.begin(), result.end(), CompareByActionOrder_OneTime);

    return result;
}

QList<CardsMoveStruct> Room::_separateMoves(QList<CardsMoveOneTimeStruct> moveOneTimes)
{
    QList<_MoveSeparateClassifier> classifiers;
    QList<QList<int> > ids;
    foreach (CardsMoveOneTimeStruct moveOneTime, moveOneTimes) {
        for (int i = 0; i < moveOneTime.card_ids.size(); i++) {
            _MoveSeparateClassifier classifier(moveOneTime, i);
            if (classifiers.contains(classifier)) {
                int pos = classifiers.indexOf(classifier);
                ids[pos].append(moveOneTime.card_ids[i]);
            } else {
                classifiers << classifier;
                QList<int> new_ids;
                new_ids << moveOneTime.card_ids[i];
                ids << new_ids;
            }
        }
    }

    QList<CardsMoveStruct> card_moves;
    int i = 0;
    QMap<ServerPlayer *, QList<int> > from_handcards;
    foreach (_MoveSeparateClassifier cls, classifiers) {
        CardsMoveStruct card_move;
        ServerPlayer *from = (ServerPlayer *)cls.m_from;
        card_move.from = cls.m_from;
        if (from && !from_handcards.keys().contains(from))
            from_handcards[from] = from->handCards();
        card_move.to = cls.m_to;
        if (card_move.from)
            card_move.from_player_name = card_move.from->objectName();
        if (card_move.to)
            card_move.to_player_name = card_move.to->objectName();
        card_move.from_place = cls.m_from_place;
        card_move.to_place = cls.m_to_place;
        card_move.from_pile_name = cls.m_from_pile_name;
        card_move.to_pile_name = cls.m_to_pile_name;
        card_move.open = cls.m_open;
        card_move.card_ids = ids.at(i);
        card_move.reason = cls.m_reason;

        if (from && from_handcards.keys().contains(from)) {
            QList<int> &move_ids = from_handcards[from];
            if (!move_ids.isEmpty()) {
                foreach(int id, card_move.card_ids)
                    move_ids.removeOne(id);
                card_move.is_last_handcard = move_ids.isEmpty();
            }
        }

        card_moves.append(card_move);
        i++;
    }
    if (card_moves.size() > 1)
        qSort(card_moves.begin(), card_moves.end(), CompareByActionOrder);
    return card_moves;
}

void Room::moveCardsAtomic(CardsMoveStruct cards_move, bool forceMoveVisible)
{
    QList<CardsMoveStruct> cards_moves;
    cards_moves.append(cards_move);
    moveCardsAtomic(cards_moves, forceMoveVisible);
}

void Room::moveCardsAtomic(QList<CardsMoveStruct> cards_moves, bool forceMoveVisible)
{
    cards_moves = _breakDownCardMoves(cards_moves);

    QList<CardsMoveOneTimeStruct> moveOneTimes = _mergeMoves(cards_moves);
    foreach (ServerPlayer *player, getAllPlayers()) {
        int i = 0;
        foreach (CardsMoveOneTimeStruct moveOneTime, moveOneTimes) {
            if (moveOneTime.card_ids.size() == 0) {
                i++;
                continue;
            }
            QVariant data = QVariant::fromValue(moveOneTime);
            thread->trigger(BeforeCardsMove, this, player, data);
            moveOneTime = data.value<CardsMoveOneTimeStruct>();
            moveOneTimes[i] = moveOneTime;
            i++;
        }
    }
    cards_moves = _separateMoves(moveOneTimes);

    notifyMoveCards(true, cards_moves, forceMoveVisible);
    // First, process remove card
    for (int i = 0; i < cards_moves.size(); i++) {
        CardsMoveStruct &cards_move = cards_moves[i];
        for (int j = 0; j < cards_move.card_ids.size(); j++) {
            int card_id = cards_move.card_ids[j];
            const Card *card = Sanguosha->getCard(card_id);

            if (cards_move.from) // Hand/Equip/Judge
                cards_move.from->removeCard(card, cards_move.from_place);

            switch (cards_move.from_place) {
            case Player::DiscardPile: m_discardPile->removeOne(card_id); break;
            case Player::DrawPile: m_drawPile->removeOne(card_id); break;
            case Player::PlaceSpecial: table_cards.removeOne(card_id); break;
            default:
                break;
            }
        }
        if (cards_move.from_place == Player::DrawPile)
            doBroadcastNotify(S_COMMAND_UPDATE_PILE, QVariant(m_drawPile->length()));
    }

    foreach(CardsMoveStruct move, cards_moves)
        updateCardsOnLose(move);

    for (int i = 0; i < cards_moves.size(); i++) {
        CardsMoveStruct &cards_move = cards_moves[i];
        for (int j = 0; j < cards_move.card_ids.size(); j++) {
            setCardMapping(cards_move.card_ids[j], (ServerPlayer *)cards_move.to, cards_move.to_place);
        }
    }
    foreach(CardsMoveStruct move, cards_moves)
        updateCardsOnGet(move);
    notifyMoveCards(false, cards_moves, forceMoveVisible);

    // Now, process add cards
    for (int i = 0; i < cards_moves.size(); i++) {
        CardsMoveStruct &cards_move = cards_moves[i];
        for (int j = 0; j < cards_move.card_ids.size(); j++) {
            int card_id = cards_move.card_ids[j];
            const Card *card = Sanguosha->getCard(card_id);
            if (forceMoveVisible && cards_move.to_place == Player::PlaceHand)
                card->setFlags("visible");
            else
                card->setFlags("-visible");
            if (cards_move.to) // Hand/Equip/Judge
                cards_move.to->addCard(card, cards_move.to_place);

            switch (cards_move.to_place) {
            case Player::DiscardPile: m_discardPile->prepend(card_id); break;
            case Player::DrawPile: m_drawPile->prepend(card_id); break;
            case Player::PlaceSpecial: table_cards.append(card_id); break;
            default:
                break;
            }
        }
    }

    //trigger event
    moveOneTimes = _mergeMoves(cards_moves);
    foreach (ServerPlayer *player, getAllPlayers()) {
        foreach (CardsMoveOneTimeStruct moveOneTime, moveOneTimes) {
            if (moveOneTime.card_ids.size() == 0) continue;
            QVariant data = QVariant::fromValue(moveOneTime);
            thread->trigger(CardsMoveOneTime, this, player, data);
        }
    }
}

QList<CardsMoveStruct> Room::_breakDownCardMoves(QList<CardsMoveStruct> &cards_moves)
{
    QList<CardsMoveStruct> all_sub_moves;
    for (int i = 0; i < cards_moves.size(); i++) {
        CardsMoveStruct &move = cards_moves[i];
        if (move.card_ids.size() == 0) continue;

        QMap<_MoveSourceClassifier, QList<int> > moveMap;
        // reassemble move sources
        for (int j = 0; j < move.card_ids.size(); j++) {
            _fillMoveInfo(move, j);
            _MoveSourceClassifier classifier(move);
            moveMap[classifier].append(move.card_ids[j]);
        }
        foreach (_MoveSourceClassifier cls, moveMap.keys()) {
            CardsMoveStruct sub_move = move;
            cls.copyTo(sub_move);
            if ((sub_move.from == sub_move.to && sub_move.from_place == sub_move.to_place)
                || sub_move.card_ids.size() == 0)
                continue;
            sub_move.card_ids = moveMap[cls];
            all_sub_moves.append(sub_move);
        }
    }
    return all_sub_moves;
}

void Room::updateCardsOnLose(const CardsMoveStruct &move)
{
    for (int i = 0; i < move.card_ids.size(); i++) {
        WrappedCard *card = qobject_cast<WrappedCard *>(getCard(move.card_ids[i]));
        if (card->isModified()) {
            if (move.to_place == Player::DiscardPile) {
                resetCard(move.card_ids[i]);
                broadcastResetCard(getPlayers(), move.card_ids[i]);
            }
        }
    }
}

void Room::updateCardsOnGet(const CardsMoveStruct &move)
{
    if (move.card_ids.isEmpty()) return;
    ServerPlayer *player = (ServerPlayer *)move.from;
    if (player != NULL && move.to_place == Player::PlaceDelayedTrick) {
        for (int i = 0; i < move.card_ids.size(); i++) {
            WrappedCard *card = qobject_cast<WrappedCard *>(getCard(move.card_ids[i]));
            const Card *engine_card = Sanguosha->getEngineCard(move.card_ids[i]);
            if (card->getSuit() != engine_card->getSuit() || card->getNumber() != engine_card->getNumber()) {
                Card *trick = Sanguosha->cloneCard(card->getRealCard());
                trick->setSuit(engine_card->getSuit());
                trick->setNumber(engine_card->getNumber());
                card->takeOver(trick);
                broadcastUpdateCard(getPlayers(), move.card_ids[i], card);
            }
        }
        return;
    }

    player = (ServerPlayer *)move.to;
    if (player != NULL && (move.to_place == Player::PlaceHand
        || move.to_place == Player::PlaceEquip
        || move.to_place == Player::PlaceJudge
        || move.to_place == Player::PlaceSpecial)) {
        QList<const Card *> cards;
        foreach(int cardId, move.card_ids)
            cards.append(getCard(cardId));
        filterCards(player, cards, true);
    }
}

bool Room::notifyMoveCards(bool isLostPhase, QList<CardsMoveStruct> &cards_moves, bool forceVisible, QList<ServerPlayer *> players)
{
    if (players.isEmpty()) players = m_players;
    // Notify clients
    int moveId;
    if (isLostPhase)
        moveId = _m_lastMovementId++;
    else
        moveId = --_m_lastMovementId;
    Q_ASSERT(_m_lastMovementId >= 0);
    foreach (ServerPlayer *player, players) {
        if (player->isOffline()) continue;
        JsonArray arg;
        arg << moveId;
        for (int i = 0; i < cards_moves.size(); i++) {
            ServerPlayer *to = NULL;
            foreach (ServerPlayer *player, m_players) {
                if (player->objectName() == cards_moves[i].to_player_name) {
                    to = player;
                    break;
                }
            }
            cards_moves[i].open = forceVisible || cards_moves[i].isRelevant(player)
                // forceVisible will override cards to be visible
                || cards_moves[i].to_place == Player::PlaceEquip
                || cards_moves[i].from_place == Player::PlaceEquip
                || cards_moves[i].to_place == Player::PlaceDelayedTrick
                || cards_moves[i].from_place == Player::PlaceDelayedTrick
                // only cards moved to hand/special can be invisible
                || cards_moves[i].from_place == Player::DiscardPile
                || cards_moves[i].to_place == Player::DiscardPile
                // any card from/to discard pile should be visible
                || cards_moves[i].from_place == Player::PlaceTable
                || cards_moves[i].to_place == Player::PlaceTable
                // any card from/to place table should be visible
                || (cards_moves[i].to_place == Player::PlaceSpecial
                && to && to->pileOpen(cards_moves[i].to_pile_name, player->objectName()))
                // pile open to specific players
                || player->hasFlag("Global_GongxinOperator");
            // the player put someone's cards to the drawpile
            arg << cards_moves[i].toVariant();
        }
        doNotify(player, isLostPhase ? S_COMMAND_LOSE_CARD : S_COMMAND_GET_CARD, arg);
    }
    return true;
}

void Room::notifySkillInvoked(ServerPlayer *player, const QString &skill_name)
{
    JsonArray args;
    args << QSanProtocol::S_GAME_EVENT_SKILL_INVOKED;
    args << player->objectName();
    args << skill_name;
    doBroadcastNotify(QSanProtocol::S_COMMAND_LOG_EVENT, args);
}

void Room::broadcastSkillInvoke(const QString &skill_name, const QString &category)
{
    JsonArray args;
    args << QSanProtocol::S_GAME_EVENT_PLAY_EFFECT;
    args << skill_name;
    args << category;
    args << -1;
    doBroadcastNotify(QSanProtocol::S_COMMAND_LOG_EVENT, args);
}

void Room::broadcastSkillInvoke(const QString &skill_name)
{
    JsonArray args;
    args << QSanProtocol::S_GAME_EVENT_PLAY_EFFECT;
    args << skill_name;
    args << true;
    args << -1;
    doBroadcastNotify(QSanProtocol::S_COMMAND_LOG_EVENT, args);
}

void Room::broadcastSkillInvoke(const QString &skill_name, int type)
{
    JsonArray args;
    args << QSanProtocol::S_GAME_EVENT_PLAY_EFFECT;
    args << skill_name;
    args << true;
    args << type;
    doBroadcastNotify(QSanProtocol::S_COMMAND_LOG_EVENT, args);
}

void Room::broadcastSkillInvoke(const QString &skill_name, bool isMale, int type)
{
    JsonArray args;
    args << QSanProtocol::S_GAME_EVENT_PLAY_EFFECT;
    args << skill_name;
    args << isMale;
    args << type;
    doBroadcastNotify(QSanProtocol::S_COMMAND_LOG_EVENT, args);
}

void Room::doLightbox(const QString &lightboxName, int duration, int pixelSize)
{
    if (Config.AIDelay == 0) return;
    doAnimate(S_ANIMATE_LIGHTBOX, lightboxName, QString("%1:%2").arg(duration).arg(pixelSize));
    thread->delay(duration / 1.2);
}

void Room::doSuperLightbox(const QString &heroName, const QString &skillName)
{
    if (Config.AIDelay == 0)
        return;

    doAnimate(S_ANIMATE_LIGHTBOX, "skill=" + heroName, skillName);
    thread->delay(4500);
}

void Room::doAnimate(QSanProtocol::AnimateType type, const QString &arg1, const QString &arg2,
    QList<ServerPlayer *> players)
{
    if (players.isEmpty())
        players = m_players;
    JsonArray arg;
    arg << (int)type;
    arg << arg1;
    arg << arg2;
    doBroadcastNotify(players, S_COMMAND_ANIMATE, arg);
}

void Room::preparePlayers()
{
    foreach (ServerPlayer *player, m_players) {
        QList<const Skill *> skills = player->getGeneral()->getSkillList();
        foreach(const Skill *skill, skills)
            player->addSkill(skill->objectName());
        if (player->getGeneral2()) {
            skills = player->getGeneral2()->getSkillList();
            foreach(const Skill *skill, skills)
                player->addSkill(skill->objectName());
        }
        player->setGender(player->getGeneral()->getGender());
    }

    JsonArray args;
    args << (int)QSanProtocol::S_GAME_EVENT_PREPARE_SKILL;
    doBroadcastNotify(QSanProtocol::S_COMMAND_LOG_EVENT, args);
}

void Room::changePlayerGeneral(ServerPlayer *player, const QString &new_general)
{
    if (player->getGeneral() != NULL) {
        foreach(const Skill *skill, player->getGeneral()->getSkillList())
            player->loseSkill(skill->objectName());        
    }
    setPlayerProperty(player, "general", new_general);
    player->setGender(player->getGeneral()->getGender());
    setPlayerProperty(player,"kingdom",player->getGeneral()->getKingdom());
    foreach(const Skill *skill, player->getVisibleSkillList())
        if (skill->isAttachedLordSkill())
            player->loseAttachLordSkill(skill->objectName());
    foreach(const Skill *skill, player->getGeneral()->getSkillList())
        player->addSkill(skill->objectName());
    filterCards(player, player->getCards("he"), true);
}

void Room::changePlayerGeneral2(ServerPlayer *player, const QString &new_general)
{
    if (player->getGeneral2() != NULL) {
        foreach(const Skill *skill, player->getGeneral2()->getSkillList())
            player->loseSkill(skill->objectName());        
    }
    setPlayerProperty(player, "general2", new_general);
    foreach(const Skill *skill, player->getVisibleSkillList())
        if (skill->isAttachedLordSkill())
            player->loseAttachLordSkill(skill->objectName());
    if (player->getGeneral2()) {
        foreach(const Skill *skill, player->getGeneral2()->getSkillList())
            player->addSkill(skill->objectName());
    }
    filterCards(player, player->getCards("he"), true);
}

void Room::filterCards(ServerPlayer *player, QList<const Card *> cards, bool refilter)
{
    if (refilter) {
        for (int i = 0; i < cards.size(); i++) {
            WrappedCard *card = qobject_cast<WrappedCard *>(getCard(cards[i]->getId()));
            if (card->isModified()) {
                int cardId = card->getId();
                resetCard(cardId);
                Player::Place place = getCardPlace(cardId);
                if (place != Player::PlaceHand) {
                    QList<ServerPlayer *> players = m_players;
                    if (place == Player::PlaceSpecial) {
                        QString pilename = player->getPileName(cardId);
                        foreach (ServerPlayer *p, m_players) {
                            if (!player->pileOpen(pilename, p->objectName()))
                                players.removeOne(p);
                        }
                    }
                    broadcastResetCard(players, cardId);
                } else {
                    notifyResetCard(player, cardId);
                }
            }
        }
    }

    QList<bool> cardChanged;
    for (int i = 0; i < cards.size(); i++)
        cardChanged.append(false);

    QSet<const Skill *> skills = player->getSkills(false, false);
    QList<const FilterSkill *> filterSkills;

    foreach (const Skill *skill, skills) {
        if (player->hasSkill(skill) && skill->inherits("FilterSkill")) {
            const FilterSkill *filter = qobject_cast<const FilterSkill *>(skill);
            Q_ASSERT(filter);
            filterSkills.append(filter);
        }
    }
    if (filterSkills.size() == 0) return;

    for (int i = 0; i < cards.size(); i++) {
        const Card *card = cards[i];
        int cardId = card->getId();
        Player::Place place = getCardPlace(cardId);
        for (int fTime = 0; fTime < filterSkills.size(); fTime++) {
            bool converged = true;
            foreach (const FilterSkill *skill, filterSkills) {
                Q_ASSERT(skill);
                if (place != Player::PlaceSpecial && skill->viewFilter(cards[i])) {
                    cards[i] = skill->viewAs(card);
                    Q_ASSERT(cards[i] != NULL);
                    converged = false;
                    cardChanged[i] = true;
                }
            }
            if (converged) break;
        }
    }

    for (int i = 0; i < cards.size(); i++) {
        int cardId = cards[i]->getId();
        Player::Place place = getCardPlace(cardId);
        if (!cardChanged[i]) continue;
        if (place == Player::PlaceHand)
            notifyUpdateCard(player, cardId, cards[i]);
        else {
            broadcastUpdateCard(m_players, cardId, cards[i]);
            if (place == Player::PlaceJudge) {
                LogMessage log;
                log.type = "#FilterJudge";
                log.arg = cards[i]->getSkillName();
                log.from = player;

                sendLog(log);
                broadcastSkillInvoke(cards[i]->getSkillName());
            }
        }
    }
}

void Room::acquireSkill(ServerPlayer *player, const Skill *skill, bool open)
{
    QString skill_name = skill->objectName();
    bool acquired = false;
    if (player->getAcquiredSkills().contains(skill_name)) acquired = true;
    player->acquireSkill(skill_name);
    if (acquired) return;

    if (skill->inherits("TriggerSkill")) {
        const TriggerSkill *trigger_skill = qobject_cast<const TriggerSkill *>(skill);
        thread->addTriggerSkill(trigger_skill);
    }
    if (skill->getFrequency() == Skill::Limited && !skill->getLimitMark().isEmpty())
        setPlayerMark(player, skill->getLimitMark(), 1);

    if (skill->isVisible()) {
        if (open) {
            JsonArray args;
            args << QSanProtocol::S_GAME_EVENT_ACQUIRE_SKILL;
            args << player->objectName();
            args << skill_name;
            doBroadcastNotify(QSanProtocol::S_COMMAND_LOG_EVENT, args);
        }

        QVariant data = skill_name;
        thread->trigger(EventAcquireSkill, this, player, data);

        foreach (const Skill *related_skill, Sanguosha->getRelatedSkills(skill_name)) {
            if (!related_skill->isVisible())
                acquireSkill(player, related_skill);
        }
    }
}

void Room::acquireSkill(ServerPlayer *player, const QString &skill_name, bool open)
{
    const Skill *skill = Sanguosha->getSkill(skill_name);
    if (skill) acquireSkill(player, skill, open);
}

void Room::setTag(const QString &key, const QVariant &value)
{
    tag.insert(key, value);
    if (scenario) scenario->onTagSet(this, key);
}

QVariant Room::getTag(const QString &key) const
{
    return tag.value(key);
}

void Room::removeTag(const QString &key)
{
    tag.remove(key);
}

void Room::setEmotion(ServerPlayer *target, const QString &emotion)
{
    JsonArray arg;
    arg << target->objectName();
    arg << (emotion.isEmpty() ? QString(".") : emotion);
    doBroadcastNotify(S_COMMAND_SET_EMOTION, arg);
}


void Room::activate(ServerPlayer *player, CardUseStruct &card_use)
{
    tryPause();

    if (player->getPhase() != Player::Play) return;
    if (player->hasFlag("Global_PlayPhaseTerminated")) {
        setPlayerFlag(player, "-Global_PlayPhaseTerminated");
        card_use.card = NULL;
        return;
    }

    notifyMoveFocus(player, S_COMMAND_PLAY_CARD);

    _m_roomState.setCurrentCardUsePattern(QString());
    _m_roomState.setCurrentCardUseReason(CardUseStruct::CARD_USE_REASON_PLAY);

    AI *ai = player->getAI();
    if (ai) {
        QElapsedTimer timer;
        timer.start();

        card_use.from = player;
        ai->activate(card_use);

        qint64 diff = Config.AIDelay - timer.elapsed();
        if (diff > 0) thread->delay(diff);
    } else {
        bool success = doRequest(player, S_COMMAND_PLAY_CARD, player->objectName(), true);
        const QVariant &clientReply = player->getClientReply();

        if (m_surrenderRequestReceived) {
            makeSurrender(player);
            if (!game_finished)
                return activate(player, card_use);
        } else {
            if (Config.EnableCheat && makeCheat(player)) {
                if (player->isAlive()) return activate(player, card_use);
                return;
            }
        }

        if (!success || clientReply.isNull()) return;

        card_use.from = player;
        if (!card_use.tryParse(clientReply, this)) {
            JsonArray use = clientReply.value<JsonArray>();
            emit room_message(tr("Card cannot be parsed:\n %1").arg(use[0].toString()));
            return;
        }
    }
    if (!card_use.isValid(QString()))
        return;
    QVariant data = QVariant::fromValue(card_use);
    thread->trigger(ChoiceMade, this, player, data);
}

void Room::askForLuckCard()
{
    tryPause();

    QList<ServerPlayer *> players;
    foreach (ServerPlayer *player, m_players) {
        if (!player->getAI()) {
            player->m_commandArgs = QVariant();
            players << player;
        }
    }
    if (players.isEmpty())
        return;

    Countdown countdown;
    countdown.max = ServerInfo.getCommandTimeout(S_COMMAND_LUCK_CARD, S_CLIENT_INSTANCE);
    countdown.type = Countdown::S_COUNTDOWN_USE_SPECIFIED;
    notifyMoveFocus(players, S_COMMAND_LUCK_CARD, countdown);

    doBroadcastRequest(players, S_COMMAND_LUCK_CARD);

    QList<ServerPlayer *> used;
    foreach (ServerPlayer *player, players) {
        const QVariant &clientReply = player->getClientReply();
        if (!player->m_isClientResponseReady || !JsonUtils::isBool(clientReply) || !clientReply.toBool())
            continue;
        used << player;
    }
    if (used.isEmpty())
        return;

    LogMessage log;
    log.type = "#UseLuckCard";
    foreach (ServerPlayer *player, used) {
        log.from = player;
        sendLog(log);
    }

    QList<int> draw_list;
    foreach (ServerPlayer *player, used) {
        draw_list << player->getHandcardNum();

        CardMoveReason reason(CardMoveReason::S_REASON_PUT, player->objectName(), "luck_card", QString());
        QList<CardsMoveStruct> moves;
        CardsMoveStruct move;
        move.from = player;
        move.from_place = Player::PlaceHand;
        move.to = NULL;
        move.to_place = Player::DrawPile;
        move.card_ids = player->handCards();
        move.reason = reason;
        moves.append(move);
        moves = _breakDownCardMoves(moves);

        QList<ServerPlayer *> tmp_list;
        tmp_list.append(player);

        notifyMoveCards(true, moves, false, tmp_list);
        for (int j = 0; j < move.card_ids.size(); j++) {
            int card_id = move.card_ids[j];
            const Card *card = Sanguosha->getCard(card_id);
            player->removeCard(card, Player::PlaceHand);
        }

        updateCardsOnLose(move);
        for (int j = 0; j < move.card_ids.size(); j++)
            setCardMapping(move.card_ids[j], NULL, Player::DrawPile);
        updateCardsOnGet(move);

        notifyMoveCards(false, moves, false, tmp_list);
        for (int j = 0; j < move.card_ids.size(); j++) {
            int card_id = move.card_ids[j];
            m_drawPile->prepend(card_id);
        }
    }
    qShuffle(*m_drawPile);
    int index = -1;
    foreach (ServerPlayer *player, used) {
        index++;
        QList<CardsMoveStruct> moves;
        CardsMoveStruct move;
        move.from = NULL;
        move.from_place = Player::DrawPile;
        move.to = player;
        move.to_place = Player::PlaceHand;
        move.card_ids = getNCards(draw_list.at(index), false);
        moves.append(move);
        moves = _breakDownCardMoves(moves);

        notifyMoveCards(true, moves, false);
        for (int j = 0; j < move.card_ids.size(); j++) {
            int card_id = move.card_ids[j];
            m_drawPile->removeOne(card_id);
        }

        updateCardsOnLose(move);
        for (int j = 0; j < move.card_ids.size(); j++)
            setCardMapping(move.card_ids[j], player, Player::PlaceHand);
        updateCardsOnGet(move);

        notifyMoveCards(false, moves, false);
        for (int j = 0; j < move.card_ids.size(); j++) {
            int card_id = move.card_ids[j];
            const Card *card = Sanguosha->getCard(card_id);
            player->addCard(card, Player::PlaceHand);
        }
    }
    doBroadcastNotify(S_COMMAND_UPDATE_PILE, QVariant(m_drawPile->length()));
}

Card::Suit Room::askForSuit(ServerPlayer *player, const QString &reason)
{
    tryPause();
    notifyMoveFocus(player, S_COMMAND_CHOOSE_SUIT);

    AI *ai = player->getAI();
    if (ai)
        return ai->askForSuit(reason);

    bool success = doRequest(player, S_COMMAND_CHOOSE_SUIT, QVariant(), true);

    Card::Suit suit = Card::AllSuits[qrand() % 4];
    if (success) {
        const QVariant &clientReply = player->getClientReply();
        QString suitStr = clientReply.toString();
        if (suitStr == "spade")
            suit = Card::Spade;
        else if (suitStr == "club")
            suit = Card::Club;
        else if (suitStr == "heart")
            suit = Card::Heart;
        else if (suitStr == "diamond")
            suit = Card::Diamond;
    }

    return suit;
}

QString Room::askForKingdom(ServerPlayer *player, const QString &reason)
{
    tryPause();
    notifyMoveFocus(player, S_COMMAND_CHOOSE_KINGDOM);

    QString result = "wei";
    AI *ai = player->getAI();
    if (ai) {
        if (reason.length() > 0)
            result = ai->askForChoice(reason, Sanguosha->getKingdoms().join("+"), QVariant());
        else
            result = ai->askForKingdom();
    } else {
        bool success = doRequest(player, S_COMMAND_CHOOSE_KINGDOM, QVariant(), true);
        const QVariant &clientReply = player->getClientReply();
        if (success && JsonUtils::isString(clientReply)) {
            QString kingdom = clientReply.toString();
            if (Sanguosha->getKingdoms().contains(kingdom))
                result = kingdom;
        }
    }

    LogMessage log;
    log.type = "#ChooseKingdom";
    log.from = player;
    log.arg = result;
    sendLog(log);

    return result;
}

bool Room::askForDiscard(ServerPlayer *player, const QString &reason, int discard_num, int min_num,
    bool optional, bool include_equip, const QString &prompt, const QString &pattern)
{
    if (!player->isAlive())
        return false;
    tryPause();
    notifyMoveFocus(player, S_COMMAND_DISCARD_CARD);
    min_num = qMin(min_num, discard_num);
    ExpPattern exp_pattern(pattern);

    if (!optional) {
        DummyCard *dummy = new DummyCard;
        dummy->deleteLater();
        QList<int> jilei_list;
        QList<const Card *> handcards = player->getHandcards();
        foreach (const Card *card, handcards) {
            if (!player->isJilei(card) && exp_pattern.match(player, card))
                dummy->addSubcard(card);
            else
                jilei_list << card->getId();
        }
        if (include_equip) {
            QList<const Card *> equips = player->getEquips();
            foreach (const Card *card, equips) {
                if (!player->isJilei(card))
                    dummy->addSubcard(card);
            }
        }

        int card_num = dummy->subcardsLength();
        if (card_num <= min_num) {
            if (card_num > 0) {
                CardMoveReason movereason;
                movereason.m_playerId = player->objectName();
                movereason.m_skillName = dummy->getSkillName();
                if (reason == "gamerule")
                    movereason.m_reason = CardMoveReason::S_REASON_RULEDISCARD;
                else
                    movereason.m_reason = CardMoveReason::S_REASON_THROW;

                throwCard(dummy, movereason, player);

                QVariant data;
                data = QString("%1:%2:%3").arg("cardDiscard").arg(reason).arg(dummy->toString());
                thread->trigger(ChoiceMade, this, player, data);
            }

            if (card_num < min_num && !jilei_list.isEmpty()) {
                JsonArray gongxinArgs;
                gongxinArgs << player->objectName();
                gongxinArgs << false;
                gongxinArgs << JsonUtils::toJsonArray(jilei_list);

                foreach (int cardId, jilei_list) {
                    WrappedCard *card = Sanguosha->getWrappedCard(cardId);
                    if (card->isModified())
                        broadcastUpdateCard(getOtherPlayers(player), cardId, card);
                    else
                        broadcastResetCard(getOtherPlayers(player), cardId);
                }

                LogMessage log;
                log.type = "$JileiShowAllCards";
                log.from = player;

                foreach(int card_id, jilei_list)
                    Sanguosha->getCard(card_id)->setFlags("visible");
                log.card_str = IntList2StringList(jilei_list).join("+");
                sendLog(log);

                doBroadcastNotify(S_COMMAND_SHOW_ALL_CARDS, gongxinArgs);
                return false;
            }
            return true;
        }
    }

    AI *ai = player->getAI();
    QList<int> to_discard;
    if (ai) {
        to_discard = ai->askForDiscard(reason, discard_num, min_num, optional, include_equip, pattern);
    } else {
        JsonArray ask_str;
        ask_str << discard_num;
        ask_str << min_num;
        ask_str << optional;
        ask_str << include_equip;
        ask_str << prompt;
        ask_str << pattern;
        bool success = doRequest(player, S_COMMAND_DISCARD_CARD, ask_str, true);

        //@todo: also check if the player does have that card!!!
        JsonArray clientReply = player->getClientReply().value<JsonArray>();
        if (!success || ((int)clientReply.size() > discard_num || (int)clientReply.size() < min_num)
            || !JsonUtils::tryParse(clientReply, to_discard)) {
            if (optional) return false;
            if (optional) return false;
            // time is up, and the server choose the cards to discard
            to_discard = player->forceToDiscard(discard_num, include_equip, true, pattern);
        }
    }

    if (to_discard.isEmpty()) return false;

    DummyCard *dummy_card = new DummyCard(to_discard);
    if (reason == "gamerule") {
        CardMoveReason mreason(CardMoveReason::S_REASON_RULEDISCARD, player->objectName(), QString(), dummy_card->getSkillName(), reason);
        throwCard(dummy_card, mreason, player);
    } else {
        CardMoveReason mreason(CardMoveReason::S_REASON_THROW, player->objectName(), QString(), dummy_card->getSkillName(), reason);
        throwCard(dummy_card, mreason, player);
    }

    QVariant data;
    data = QString("%1:%2:%3").arg("cardDiscard").arg(reason).arg(dummy_card->toString());
    thread->trigger(ChoiceMade, this, player, data);

    delete dummy_card;
    return true;
}

const Card *Room::askForExchange(ServerPlayer *player, const QString &reason, int discard_num, int min_num,
    bool include_equip, const QString &prompt, bool optional, const QString &pattern)
{
    if (!player->isAlive())
        return NULL;
    tryPause();
    notifyMoveFocus(player, S_COMMAND_EXCHANGE_CARD);
    min_num = qMin(min_num, discard_num);

    if (player->isNude()) return NULL;

    if (!optional && player->getCardCount(include_equip) <= min_num) {
        DummyCard *card = new DummyCard;
        QString flag = include_equip ? "he" : "h";
        card->addSubcards(player->getCards(flag));
        return card;
    }

    QList<int> to_exchange;
    AI *ai = player->getAI();
    if (ai) {
        // share the same callback interface
        player->setFlags("Global_AIDiscardExchanging");
        to_exchange = ai->askForDiscard(reason, discard_num, min_num, optional, include_equip, pattern);
        player->setFlags("-Global_AIDiscardExchanging");
    } else {
        JsonArray exchange_str;
        exchange_str << discard_num;
        exchange_str << min_num;
        exchange_str << include_equip;
        exchange_str << prompt;
        exchange_str << optional;
        exchange_str << pattern;

        bool success = doRequest(player, S_COMMAND_EXCHANGE_CARD, exchange_str, true);
        //@todo: also check if the player does have that card!!!
        JsonArray clientReply = player->getClientReply().value<JsonArray>();
        if (!success || (int)clientReply.size() > discard_num || (int)clientReply.size() < min_num
            || !JsonUtils::tryParse(clientReply, to_exchange)) {
            if (optional) return NULL;
            to_exchange = player->forceToDiscard(discard_num, include_equip, false, pattern);
        }
    }

    if (to_exchange.isEmpty()) return NULL;

    DummyCard *card = new DummyCard(to_exchange);
    return card;
}

void Room::setCardMapping(int card_id, ServerPlayer *owner, Player::Place place)
{
    owner_map.insert(card_id, owner);
    place_map.insert(card_id, place);
}

ServerPlayer *Room::getCardOwner(int card_id) const
{
    return owner_map.value(card_id);
}

Player::Place Room::getCardPlace(int card_id) const
{
    if (card_id < 0) return Player::PlaceUnknown;
    return place_map.value(card_id);
}

ServerPlayer *Room::getLord() const
{
    ServerPlayer *the_lord = m_players.first();
    if (the_lord->getRole() == "lord")
        return the_lord;

    foreach (ServerPlayer *player, m_players) {
        if (player->getRole() == "lord")
            return player;
    }

    return NULL;
}

void Room::askForGuanxing(ServerPlayer *zhuge, const QList<int> &cards, GuanxingType guanxing_type)
{
    QList<int> top_cards, bottom_cards;
    tryPause();
    notifyMoveFocus(zhuge, S_COMMAND_SKILL_GUANXING);

    AI *ai = zhuge->getAI();
    if (ai) {
        ai->askForGuanxing(cards, top_cards, bottom_cards, (int)guanxing_type);
    } else if (guanxing_type == GuanxingUpOnly && cards.length() == 1) {
        top_cards = cards;
    } else if (guanxing_type == GuanxingDownOnly && cards.length() == 1) {
        bottom_cards = cards;
    } else {
        JsonArray guanxingArgs;
        guanxingArgs << JsonUtils::toJsonArray(cards);
        guanxingArgs << (guanxing_type != GuanxingBothSides);
        bool success = doRequest(zhuge, S_COMMAND_SKILL_GUANXING, guanxingArgs, true);
        if (!success) {
            foreach (int card_id, cards) {
                if (guanxing_type == GuanxingDownOnly)
                    m_drawPile->append(card_id);
                else
                    m_drawPile->prepend(card_id);
            }
            return;
        }
        JsonArray clientReply = zhuge->getClientReply().value<JsonArray>();
        if (clientReply.size() == 2) {
            success &= JsonUtils::tryParse(clientReply[0], top_cards);
            success &= JsonUtils::tryParse(clientReply[1], bottom_cards);
            if (guanxing_type == GuanxingDownOnly) {
                bottom_cards = top_cards;
                top_cards.clear();
            }
        }
    }

    bool length_equal = top_cards.length() + bottom_cards.length() == cards.length();
    bool result_equal = top_cards.toSet() + bottom_cards.toSet() == cards.toSet();
    if (!length_equal || !result_equal) {
        if (guanxing_type == GuanxingDownOnly) {
            bottom_cards = cards;
            top_cards.clear();
        } else {
            top_cards = cards;
            bottom_cards.clear();
        }
    }

    if (guanxing_type == GuanxingBothSides) {
        LogMessage log;
        log.type = "#GuanxingResult";
        log.from = zhuge;
        log.arg = QString::number(top_cards.length());
        log.arg2 = QString::number(bottom_cards.length());
        sendLog(log);
    }

    if (!top_cards.isEmpty()) {
        LogMessage log;
        log.type = "$GuanxingTop";
        log.from = zhuge;
        log.card_str = IntList2StringList(top_cards).join("+");
        sendLog(log, zhuge);
    }
    if (!bottom_cards.isEmpty()) {
        LogMessage log;
        log.type = "$GuanxingBottom";
        log.from = zhuge;
        log.card_str = IntList2StringList(bottom_cards).join("+");
        sendLog(log, zhuge);
    }

    QListIterator<int> i(top_cards);
    i.toBack();
    while (i.hasPrevious())
        m_drawPile->prepend(i.previous());

    i = bottom_cards;
    while (i.hasNext())
        m_drawPile->append(i.next());

    doBroadcastNotify(S_COMMAND_UPDATE_PILE, QVariant(m_drawPile->length()));
}

void Room::returnToTopDrawPile(const QList<int> &cards)
{
    QListIterator<int> i(cards);
    i.toBack();
    while (i.hasPrevious()) {
        int id = i.previous();
        setCardMapping(id, NULL, Player::DrawPile);
        m_drawPile->prepend(id);
    }
    doBroadcastNotify(S_COMMAND_UPDATE_PILE, QVariant(m_drawPile->length()));
}

int Room::doGongxin(ServerPlayer *shenlvmeng, ServerPlayer *target, QList<int> enabled_ids, QString skill_name)
{
    Q_ASSERT(!target->isKongcheng());
    tryPause();
    notifyMoveFocus(shenlvmeng, S_COMMAND_SKILL_GONGXIN);

    LogMessage log;
    log.type = "$ViewAllCards";
    log.from = shenlvmeng;
    log.to << target;
    log.card_str = IntList2StringList(target->handCards()).join("+");
    sendLog(log, shenlvmeng);

    QVariant decisionData = QVariant::fromValue("viewCards:" + shenlvmeng->objectName() + ":" + target->objectName());
    thread->trigger(ChoiceMade, this, shenlvmeng, decisionData);

    shenlvmeng->tag[skill_name] = QVariant::fromValue(target);
    int card_id;
    AI *ai = shenlvmeng->getAI();
    if (ai) {
        QList<int> hearts;
        foreach (int id, target->handCards()) {
            if (Sanguosha->getCard(id)->getSuit() == Card::Heart)
                hearts << id;
        }
        if (enabled_ids.isEmpty()) {
            shenlvmeng->tag.remove(skill_name);
            return -1;
        }
        card_id = ai->askForAG(enabled_ids, true, skill_name);
        if (card_id == -1) {
            shenlvmeng->tag.remove(skill_name);
            return -1;
        }
    } else {
        foreach (int cardId, target->handCards()) {
            WrappedCard *card = Sanguosha->getWrappedCard(cardId);
            if (card->isModified())
                notifyUpdateCard(shenlvmeng, cardId, card);
            else
                notifyResetCard(shenlvmeng, cardId);
        }

        JsonArray gongxinArgs;
        gongxinArgs << target->objectName();
        gongxinArgs << true;
        gongxinArgs << JsonUtils::toJsonArray(target->handCards());
        gongxinArgs << JsonUtils::toJsonArray(enabled_ids);
        bool success = doRequest(shenlvmeng, S_COMMAND_SKILL_GONGXIN, gongxinArgs, true);
        const QVariant &clientReply = shenlvmeng->getClientReply();
        if (!success || !JsonUtils::isNumber(clientReply) || !target->handCards().contains(clientReply.toInt())) {
            shenlvmeng->tag.remove(skill_name);
            return -1;
        }

        card_id = clientReply.toInt();
    }
    return card_id; // Do remember to remove the tag later!
}

const Card *Room::askForPindian(ServerPlayer *player, ServerPlayer *from, ServerPlayer *to, const QString &reason)
{
    if (!from->isAlive() || !to->isAlive())
        return NULL;
    Q_ASSERT(!player->isKongcheng());
    tryPause();
    notifyMoveFocus(player, S_COMMAND_PINDIAN);

    if (player->getHandcardNum() == 1)
        return player->getHandcards().first();

    AI *ai = player->getAI();
    if (ai) {
        thread->delay();
        return ai->askForPindian(from, reason);
    }

    bool success = doRequest(player, S_COMMAND_PINDIAN, JsonArray() << from->objectName() << to->objectName(), true);

    JsonArray clientReply = player->getClientReply().value<JsonArray>();
    if (!success || clientReply.isEmpty() || !JsonUtils::isString(clientReply[0])) {
        int card_id = player->getRandomHandCardId();
        return Sanguosha->getCard(card_id);
    } else {
        const Card *card = Card::Parse(clientReply[0].toString());
        if (card->isVirtualCard()) {
            const Card *real_card = Sanguosha->getCard(card->getEffectiveId());
            delete card;
            return real_card;
        } else
            return card;
    }
}

QList<const Card *> Room::askForPindianRace(ServerPlayer *from, ServerPlayer *to, const QString &reason)
{
    if (!from->isAlive() || !to->isAlive())
        return QList<const Card *>() << NULL << NULL;
    Q_ASSERT(!from->isKongcheng() && !to->isKongcheng());
    tryPause();
    Countdown countdown;
    countdown.max = ServerInfo.getCommandTimeout(S_COMMAND_PINDIAN, S_CLIENT_INSTANCE);
    countdown.type = Countdown::S_COUNTDOWN_USE_SPECIFIED;
    notifyMoveFocus(QList<ServerPlayer *>() << from << to, S_COMMAND_PINDIAN, countdown);

    const Card *from_card = NULL, *to_card = NULL;

    if (from->getHandcardNum() == 1)
        from_card = from->getHandcards().first();
    if (to->getHandcardNum() == 1)
        to_card = to->getHandcards().first();

    AI *ai;
    if (!from_card) {
        ai = from->getAI();
        if (ai)
            from_card = ai->askForPindian(from, reason);
    }
    if (!to_card) {
        ai = to->getAI();
        if (ai)
            to_card = ai->askForPindian(from, reason);
    }
    if (from_card && to_card) {
        thread->delay();
        return QList<const Card *>() << from_card << to_card;
    }

    QList<ServerPlayer *> players;
    if (!from_card) {
        JsonArray arr;
        arr << from->objectName() << to->objectName();
        from->m_commandArgs = arr;
        players << from;
    }
    if (!to_card) {
        JsonArray arr;
        arr << from->objectName() << to->objectName();
        to->m_commandArgs = arr;
        players << to;
    }

    doBroadcastRequest(players, S_COMMAND_PINDIAN);

    foreach (ServerPlayer *player, players) {
        const Card *c = NULL;
        JsonArray clientReply = player->getClientReply().value<JsonArray>();
        if (!player->m_isClientResponseReady || clientReply.isEmpty() || !JsonUtils::isString(clientReply[0])) {
            int card_id = player->getRandomHandCardId();
            c = Sanguosha->getCard(card_id);
        } else {
            const Card *card = Card::Parse(clientReply[0].toString());
            if (card == NULL) {
                int card_id = player->getRandomHandCardId();
                c = Sanguosha->getCard(card_id);
            } else if (card->isVirtualCard()) {
                const Card *real_card = Sanguosha->getCard(card->getEffectiveId());
                delete card;
                c = real_card;
            } else
                c = card;
        }
        if (player == from)
            from_card = c;
        else
            to_card = c;
    }
    return QList<const Card *>() << from_card << to_card;
}

ServerPlayer *Room::askForPlayerChosen(ServerPlayer *player, const QList<ServerPlayer *> &targets, const QString &skillName,
    const QString &prompt, bool optional, bool notify_skill)
{
    if (targets.isEmpty()) {
        Q_ASSERT(optional);
        return NULL;
    } else if (targets.length() == 1 && !optional) {
        QVariant data = QString("%1:%2:%3").arg("playerChosen").arg(skillName).arg(targets.first()->objectName());
        thread->trigger(ChoiceMade, this, player, data);
        return targets.first();
    }

    tryPause();
    notifyMoveFocus(player, S_COMMAND_CHOOSE_PLAYER);
    AI *ai = player->getAI();
    ServerPlayer *choice = NULL;
    if (ai) {
        choice = ai->askForPlayerChosen(targets, skillName);
        if (choice && notify_skill)
            thread->delay();
    } else {
        JsonArray req;
        JsonArray req_targets;
        foreach(ServerPlayer *target, targets)
            req_targets << target->objectName();
        req << QVariant(req_targets);
        req << skillName;
        req << prompt;
        req << optional;
        bool success = doRequest(player, S_COMMAND_CHOOSE_PLAYER, req, true);

        const QVariant &clientReply = player->getClientReply();
        if (success && JsonUtils::isString(clientReply))
            choice = findChild<ServerPlayer *>(clientReply.toString());
    }
    if (choice && !targets.contains(choice))
        choice = NULL;
    if (choice == NULL && !optional)
        choice = targets.at(qrand() % targets.length());
    if (choice) {
        if (notify_skill) {
            notifySkillInvoked(player, skillName);
            QVariant decisionData = QVariant::fromValue("skillInvoke:" + skillName + ":yes");
            thread->trigger(ChoiceMade, this, player, decisionData);

            doAnimate(S_ANIMATE_INDICATE, player->objectName(), choice->objectName());
            LogMessage log;
            log.type = "#ChoosePlayerWithSkill";
            log.from = player;
            log.to << choice;
            log.arg = skillName;
            sendLog(log);
        }
        QVariant data = QString("%1:%2:%3").arg("playerChosen").arg(skillName).arg(choice->objectName());
        thread->trigger(ChoiceMade, this, player, data);
    }
    return choice;
}

void Room::_setupChooseGeneralRequestArgs(ServerPlayer *player)
{
    JsonArray options = JsonUtils::toJsonArray(player->getSelected()).value<JsonArray>();
    if (!Config.EnableBasara) {
        if (getLord())
            options.append(QString("%1(lord)").arg(getLord()->getGeneralName()));
    } else {
        options.append("anjiang(lord)");
    }
    player->m_commandArgs = options;
}

QString Room::askForGeneral(ServerPlayer *player, const QStringList &generals, QString default_choice)
{
    tryPause();
    notifyMoveFocus(player, S_COMMAND_CHOOSE_GENERAL);

    if (generals.length() == 1)
        return generals.first();

    if (default_choice.isEmpty())
        default_choice = generals.at(qrand() % generals.length());

    if (player->isOnline()) {
        JsonArray options = JsonUtils::toJsonArray(generals).value<JsonArray>();
        bool success = doRequest(player, S_COMMAND_CHOOSE_GENERAL, options, true);

        QVariant clientResponse = player->getClientReply();
        bool free = Config.FreeChoose || mode.startsWith("_mini_") || mode == "custom_scenario";
        if (!success || !JsonUtils::isString(clientResponse) || (!free && !generals.contains(clientResponse.toString())))
            return default_choice;
        else
            return clientResponse.toString();
    }

    return default_choice;
}

QString Room::askForGeneral(ServerPlayer *player, const QString &generals, QString default_choice)
{
    return askForGeneral(player, generals.split("+"), default_choice); // For Lua only!!!
}

bool Room::makeCheat(ServerPlayer *player)
{
    JsonArray arg = player->m_cheatArgs.value<JsonArray>();
    player->m_cheatArgs = QVariant();
    if (arg.isEmpty() || !JsonUtils::isNumber(arg[0])) return false;

    CheatCode code = (CheatCode)arg[0].toInt();
    if (code == S_CHEAT_KILL_PLAYER) {
        JsonArray arg1 = arg[1].value<JsonArray>();
        if (!JsonUtils::isStringArray(arg1, 0, 1)) return false;
        makeKilling(arg1[0].toString(), arg1[1].toString());

    } else if (code == S_CHEAT_MAKE_DAMAGE) {
        JsonArray arg1 = arg[1].value<JsonArray>();
        if (arg1.size() != 4 || !JsonUtils::isStringArray(arg1, 0, 1)
            || !JsonUtils::isNumber(arg1[2]) || !JsonUtils::isNumber(arg1[3]))
            return false;
        makeDamage(arg1[0].toString(), arg1[1].toString(),
            (QSanProtocol::CheatCategory)arg1[2].toInt(), arg1[3].toInt());

    } else if (code == S_CHEAT_REVIVE_PLAYER) {
        if (!JsonUtils::isString(arg[1])) return false;
        makeReviving(arg[1].toString());

    } else if (code == S_CHEAT_RUN_SCRIPT) {
        if (!JsonUtils::isString(arg[1])) return false;
        QByteArray data = QByteArray::fromBase64(arg[1].toString().toLatin1());
        data = qUncompress(data);
        doScript(data);

    } else if (code == S_CHEAT_GET_ONE_CARD) {
        if (!JsonUtils::isNumber(arg[1])) return false;
        int card_id = arg[1].toInt();

        LogMessage log;
        log.type = "$CheatCard";
        log.from = player;
        log.card_str = QString::number(card_id);
        sendLog(log);

        obtainCard(player, card_id);
    } else if (code == S_CHEAT_CHANGE_GENERAL) {
        if (!JsonUtils::isString(arg[1]) || !JsonUtils::isBool(arg[2]))
            return false;
        QString generalName = arg[1].toString();
        bool isSecondaryHero = arg[2].toBool();
        changeHero(player, generalName, false, true, isSecondaryHero);
    }

    return true;
}

void Room::makeDamage(const QString &source, const QString &target, QSanProtocol::CheatCategory nature, int point)
{
    ServerPlayer *sourcePlayer = findChild<ServerPlayer *>(source);
    ServerPlayer *targetPlayer = findChild<ServerPlayer *>(target);
    if (targetPlayer == NULL) return;

    if (nature == S_CHEAT_HP_LOSE) {
        loseHp(targetPlayer, point);
        return;
    } else if (nature == S_CHEAT_MAX_HP_LOSE) {
        loseMaxHp(targetPlayer, point);
        return;
    } else if (nature == S_CHEAT_HP_RECOVER) {
        recover(targetPlayer, RecoverStruct(sourcePlayer, NULL, point));
        return;
    } else if (nature == S_CHEAT_MAX_HP_RESET) {
        setPlayerProperty(targetPlayer, "maxhp", point);
        return;
    }

    static QMap<QSanProtocol::CheatCategory, DamageStruct::Nature> nature_map;
    if (nature_map.isEmpty()) {
        nature_map[S_CHEAT_NORMAL_DAMAGE] = DamageStruct::Normal;
        nature_map[S_CHEAT_THUNDER_DAMAGE] = DamageStruct::Thunder;
        nature_map[S_CHEAT_FIRE_DAMAGE] = DamageStruct::Fire;
    }

    if (targetPlayer == NULL) return;
    this->damage(DamageStruct("cheat", sourcePlayer, targetPlayer, point, nature_map[nature]));
}

void Room::makeKilling(const QString &killerName, const QString &victimName)
{
    ServerPlayer *killer = NULL, *victim = NULL;

    killer = findChild<ServerPlayer *>(killerName);
    victim = findChild<ServerPlayer *>(victimName);

    if (victim == NULL) return;
    if (killer == NULL) return killPlayer(victim);

    DamageStruct damage("cheat", killer, victim);
    killPlayer(victim, &damage);
}

void Room::makeReviving(const QString &name)
{
    ServerPlayer *player = findChild<ServerPlayer *>(name);
    Q_ASSERT(player);
    revivePlayer(player);
    removeTag("HpChangedData");
    setPlayerProperty(player, "maxhp", player->getGeneralMaxHp());
    setPlayerProperty(player, "hp", player->getMaxHp());
}

void Room::fillAG(const QList<int> &card_ids, ServerPlayer *who, const QList<int> &disabled_ids)
{
    JsonArray arg;
    arg << JsonUtils::toJsonArray(card_ids);
    arg << JsonUtils::toJsonArray(disabled_ids);

    m_fillAGarg = arg;

    if (who)
        doNotify(who, S_COMMAND_FILL_AMAZING_GRACE, arg);
    else
        doBroadcastNotify(S_COMMAND_FILL_AMAZING_GRACE, arg);
}

void Room::takeAG(ServerPlayer *player, int card_id, bool move_cards, QList<ServerPlayer *> to_notify)
{
    if (to_notify.isEmpty()) to_notify = getAllPlayers(true);

    JsonArray arg;
    arg << (player ? QVariant(player->objectName()) : QVariant());
    arg << card_id;
    arg << move_cards;

    if (player) {
        CardsMoveOneTimeStruct moveOneTime;
        if (move_cards) {
            CardsMoveOneTimeStruct move;
            move.from = NULL;
            move.from_places << Player::DrawPile;
            move.to = player;
            move.to_place = Player::PlaceHand;
            move.card_ids << card_id;
            QVariant data = QVariant::fromValue(move);
            foreach(ServerPlayer *p, getAllPlayers())
                thread->trigger(BeforeCardsMove, this, p, data);
            move = data.value<CardsMoveOneTimeStruct>();
            moveOneTime = move;

            if (move.card_ids.length() > 0) {
                player->addCard(Sanguosha->getCard(card_id), Player::PlaceHand);
                setCardMapping(card_id, player, Player::PlaceHand);
                Sanguosha->getCard(card_id)->setFlags("visible");
                QList<const Card *> cards;
                cards << Sanguosha->getCard(card_id);
                filterCards(player, cards, false);
            } else {
                arg[2] = false;
            }
        }
        foreach(ServerPlayer *p, to_notify)
            doNotify(p, S_COMMAND_TAKE_AMAZING_GRACE, arg);
        if (move_cards && moveOneTime.card_ids.length() > 0) {
            QVariant data = QVariant::fromValue(moveOneTime);
            foreach(ServerPlayer *p, getAllPlayers())
                thread->trigger(CardsMoveOneTime, this, p, data);
        }
    } else {
        foreach(ServerPlayer *p, to_notify)
            doNotify(p, S_COMMAND_TAKE_AMAZING_GRACE, arg);
        if (!move_cards) return;
        LogMessage log;
        log.type = "$EnterDiscardPile";
        log.card_str = QString::number(card_id);
        sendLog(log);

        m_discardPile->prepend(card_id);
        setCardMapping(card_id, NULL, Player::DiscardPile);
    }
    JsonArray takeagargs = m_takeAGargs.value<JsonArray>();
    takeagargs << arg;
    m_takeAGargs = takeagargs;
}

void Room::clearAG(ServerPlayer *player)
{
    m_fillAGarg = QVariant();
    m_takeAGargs = QVariant();
    if (player)
        doNotify(player, S_COMMAND_CLEAR_AMAZING_GRACE, QVariant());
    else
        doBroadcastNotify(S_COMMAND_CLEAR_AMAZING_GRACE, QVariant());
}

void Room::provide(const Card *card)
{
    Q_ASSERT(provided == NULL);
    Q_ASSERT(!has_provided);

    provided = card;
    has_provided = true;
}

QList<ServerPlayer *> Room::getLieges(const QString &kingdom, ServerPlayer *lord) const
{
    QList<ServerPlayer *> lieges;
    foreach (ServerPlayer *player, getAllPlayers()) {
        if (player != lord && player->getKingdom() == kingdom)
            lieges << player;
    }

    return lieges;
}

void Room::sendLog(const LogMessage &log, QList<ServerPlayer *> players)
{
    if (log.type.isEmpty())
        return;
    if (players.isEmpty())
        doBroadcastNotify(QSanProtocol::S_COMMAND_LOG_SKILL, log.toVariant());
    else
        doBroadcastNotify(players, QSanProtocol::S_COMMAND_LOG_SKILL, log.toVariant());
}

void Room::sendLog(const LogMessage &log, ServerPlayer *player)
{
    if (log.type.isEmpty())
        return;
    doNotify(player, QSanProtocol::S_COMMAND_LOG_SKILL, log.toVariant());
}

void Room::sendCompulsoryTriggerLog(ServerPlayer *player, const QString &skill_name, bool notify_skill)
{
    LogMessage log;
    log.type = "#TriggerSkill";
    log.arg = skill_name;
    log.from = player;
    sendLog(log);
    if (notify_skill)
        notifySkillInvoked(player, skill_name);
}

void Room::showCard(ServerPlayer *player, int card_id, ServerPlayer *only_viewer)
{
    if (getCardOwner(card_id) != player) return;

    tryPause();
    notifyMoveFocus(player);
    JsonArray show_arg;
    show_arg << player->objectName();
    show_arg << card_id;

    WrappedCard *card = Sanguosha->getWrappedCard(card_id);
    bool modified = card->isModified();
    if (only_viewer) {
        QList<ServerPlayer *>players;
        players << only_viewer << player;
        if (modified)
            notifyUpdateCard(only_viewer, card_id, card);
        else
            notifyResetCard(only_viewer, card_id);
        doBroadcastNotify(players, S_COMMAND_SHOW_CARD, show_arg);
    } else {
        if (card_id > 0)
            Sanguosha->getCard(card_id)->setFlags("visible");
        if (modified)
            broadcastUpdateCard(getOtherPlayers(player), card_id, card);
        else
            broadcastResetCard(getOtherPlayers(player), card_id);
        doBroadcastNotify(S_COMMAND_SHOW_CARD, show_arg);
    }
}

void Room::showAllCards(ServerPlayer *player, ServerPlayer *to)
{
    if (player->isKongcheng())
        return;
    tryPause();

    JsonArray gongxinArgs;
    gongxinArgs << player->objectName();
    gongxinArgs << false;
    gongxinArgs << JsonUtils::toJsonArray(player->handCards());

    bool isUnicast = (to != NULL);

    foreach (int cardId, player->handCards()) {
        WrappedCard *card = Sanguosha->getWrappedCard(cardId);
        if (card->isModified()) {
            if (isUnicast)
                notifyUpdateCard(to, cardId, card);
            else
                broadcastUpdateCard(getOtherPlayers(player), cardId, card);
        } else {
            if (isUnicast)
                notifyResetCard(to, cardId);
            else
                broadcastResetCard(getOtherPlayers(player), cardId);
        }
    }

    if (isUnicast) {
        LogMessage log;
        log.type = "$ViewAllCards";
        log.from = to;
        log.to << player;
        log.card_str = IntList2StringList(player->handCards()).join("+");
        sendLog(log, to);

        QVariant decisionData = QVariant::fromValue("viewCards:" + to->objectName() + ":" + player->objectName());
        thread->trigger(ChoiceMade, this, to, decisionData);

        doNotify(to, S_COMMAND_SHOW_ALL_CARDS, gongxinArgs);
    } else {
        LogMessage log;
        log.type = "$ShowAllCards";
        log.from = player;
        foreach(int card_id, player->handCards())
            Sanguosha->getCard(card_id)->setFlags("visible");
        log.card_str = IntList2StringList(player->handCards()).join("+");
        sendLog(log);

        doBroadcastNotify(getOtherPlayers(player), S_COMMAND_SHOW_ALL_CARDS, gongxinArgs);
    }
}

void Room::retrial(const Card *card, ServerPlayer *player, JudgeStruct *judge, const QString &skill_name, bool exchange)
{
    if (card == NULL) return;

    bool triggerResponded = getCardOwner(card->getEffectiveId()) == player;
    bool isHandcard = (triggerResponded && getCardPlace(card->getEffectiveId()) == Player::PlaceHand);

    const Card *oldJudge = judge->card;
    judge->card = Sanguosha->getCard(card->getEffectiveId());
    ServerPlayer *rebyre = judge->retrial_by_response;
    judge->retrial_by_response = player;

    CardResponseStruct resp(card, judge->who);
    resp.m_isHandcard = isHandcard;
    resp.m_isRetrial = true;
    QVariant data = QVariant::fromValue(resp);

    if (triggerResponded)
        thread->trigger(PreCardResponded, this, player, data);

    CardsMoveStruct move1(QList<int>(),
        judge->who,
        Player::PlaceJudge,
        CardMoveReason(CardMoveReason::S_REASON_RETRIAL,
        player->objectName(),
        skill_name,
        QString()));

    move1.card_ids.append(card->getEffectiveId());
    int reasonType = exchange ? CardMoveReason::S_REASON_OVERRIDE : CardMoveReason::S_REASON_JUDGEDONE;
    CardMoveReason reason(reasonType,
        player->objectName(),
        exchange ? skill_name : QString(), QString());
    if (rebyre)
        reason.m_extraData = QVariant::fromValue(rebyre);
    CardsMoveStruct move2(QList<int>(),
        judge->who,
        exchange ? player : NULL,
        Player::PlaceUnknown,
        exchange ? Player::PlaceHand : Player::DiscardPile,
        reason);

    move2.card_ids.append(oldJudge->getEffectiveId());

    LogMessage log;
    log.type = "$ChangedJudge";
    log.arg = skill_name;
    log.from = player;
    log.to << judge->who;
    log.card_str = QString::number(card->getEffectiveId());
    sendLog(log);

    QList<CardsMoveStruct> moves;
    moves.append(move1);
    moves.append(move2);
    moveCardsAtomic(moves, true);
    judge->updateResult();

    if (triggerResponded)
        thread->trigger(CardResponded, this, player, data);
}

bool Room::askForYiji(ServerPlayer *guojia, QList<int> &cards, const QString &skill_name,
    bool is_preview, bool visible, bool optional, int max_num,
    QList<ServerPlayer *> players, CardMoveReason reason, const QString &prompt,
    bool notify_skill)
{
    if (max_num == -1)
        max_num = cards.length();
    if (players.isEmpty())
        players = getOtherPlayers(guojia);
    if (cards.isEmpty() || max_num == 0)
        return false;
    if (reason.m_reason == CardMoveReason::S_REASON_UNKNOWN) {
        reason.m_playerId = guojia->objectName();
        // when we use ? : here, compiling error occurs under debug mode...
        if (is_preview)
            reason.m_reason = CardMoveReason::S_REASON_PREVIEWGIVE;
        else
            reason.m_reason = CardMoveReason::S_REASON_GIVE;
    }
    tryPause();
    notifyMoveFocus(guojia, S_COMMAND_SKILL_YIJI);

    ServerPlayer *target = NULL;

    QList<int> ids;
    AI *ai = guojia->getAI();
    if (ai) {
        int card_id;
        ServerPlayer *who = ai->askForYiji(cards, skill_name, card_id);
        if (!who)
            return false;
        else {
            target = who;
            ids << card_id;
        }
    } else {
        JsonArray arg;
        arg << JsonUtils::toJsonArray(cards);
        arg << optional;
        arg << max_num;
        JsonArray player_names;
        foreach(ServerPlayer *player, players)
            player_names << player->objectName();
        arg << QVariant(player_names);
        if (!prompt.isEmpty())
            arg << prompt;
        bool success = doRequest(guojia, S_COMMAND_SKILL_YIJI, arg, true);

        //Validate client response
        JsonArray clientReply = guojia->getClientReply().value<JsonArray>();
        if (!success || clientReply.size() != 2)
            return false;

        if (!JsonUtils::tryParse(clientReply[0], ids) || !JsonUtils::isString(clientReply[1]))
            return false;

        foreach(int id, ids)
            if (!cards.contains(id))
                return false;

        ServerPlayer *who = findChild<ServerPlayer *>(clientReply[1].toString());
        if (!who)
            return false;
        else
            target = who;
    }
    Q_ASSERT(target != NULL);

    DummyCard *dummy_card = new DummyCard;
    foreach (int card_id, ids) {
        cards.removeOne(card_id);
        dummy_card->addSubcard(card_id);
    }

    QVariant decisionData = QVariant::fromValue(QString("Yiji:%1:%2:%3:%4")
        .arg(skill_name).arg(guojia->objectName()).arg(target->objectName())
        .arg(IntList2StringList(ids).join("+")));
    thread->trigger(ChoiceMade, this, guojia, decisionData);

    if (notify_skill) {
        LogMessage log;
        log.type = "#InvokeSkill";
        log.from = guojia;
        log.arg = skill_name;
        sendLog(log);

        const Skill *skill = Sanguosha->getSkill(skill_name);
        if (skill)
            broadcastSkillInvoke(skill_name, skill->getEffectIndex(target, dummy_card));
        notifySkillInvoked(guojia, skill_name);
    }

    guojia->setFlags("Global_GongxinOperator");
    moveCardTo(dummy_card, target, Player::PlaceHand, reason, visible);
    guojia->setFlags("-Global_GongxinOperator");
    delete dummy_card;

    return true;
}

QString Room::generatePlayerName()
{
    static unsigned int id = 0;
    id++;
    return QString("sgs%1").arg(id);
}

QString Room::askForOrder(ServerPlayer *player, const QString &default_choice)
{
    tryPause();
    notifyMoveFocus(player, S_COMMAND_CHOOSE_ORDER);

    if (player->getAI())
        return default_choice;

    bool success = doRequest(player, S_COMMAND_CHOOSE_ORDER, (int)S_REASON_CHOOSE_ORDER_TURN, true);

    QVariant clientReply = player->getClientReply();
    if (success && JsonUtils::isNumber(clientReply))
        return ((Game3v3Camp)clientReply.toInt() == S_CAMP_WARM) ? "warm" : "cool";
    return default_choice;
}

QString Room::askForRole(ServerPlayer *player, const QStringList &roles, const QString &scheme)
{
    tryPause();
    notifyMoveFocus(player, S_COMMAND_CHOOSE_ROLE_3V3);

    QStringList squeezed = roles.toSet().toList();

    JsonArray arg;
    arg << scheme << JsonUtils::toJsonArray(squeezed);
    bool success = doRequest(player, S_COMMAND_CHOOSE_ROLE_3V3, arg, true);
    QVariant clientReply = player->getClientReply();
    QString result = "abstain";
    if (success && JsonUtils::isString(clientReply))
        result = clientReply.toString();
    return result;
}

void Room::networkDelayTestCommand(ServerPlayer *player, const QVariant &)
{
    qint64 delay = player->endNetworkDelayTest();
    QString reportStr = tr("<font color=#EEB422>The network delay of player <b>%1</b> is %2 milliseconds.</font>")
        .arg(player->screenName()).arg(QString::number(delay));
    speakCommand(player, QVariant(reportStr.toUtf8().toBase64()));
    return ;
}

void Room::sortByActionOrder(QList<ServerPlayer *> &players)
{
    if (players.length() > 1)
        qSort(players.begin(), players.end(), ServerPlayer::CompareByActionOrder);
}

int Room::getBossModeExpMult(int level) const
{
    lua_getglobal(L, "bossModeExpMult");
    lua_pushinteger(L, level);
    int ret = lua_pcall(L, 1, 1, 0);
    int res = 0;
    if (ret == 0) {
        res = lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    return res;
}
