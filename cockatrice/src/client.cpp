#include <QTimer>
#include "client.h"

// Message structure for server events:
// {"private","public"}|PlayerId|PlayerName|EventType|EventData

QHash<QString, ServerEventType> ServerEventData::eventHash;

ServerEventData::ServerEventData(const QString &line)
{
	if (eventHash.isEmpty()) {
		eventHash.insert("player_id", eventPlayerId);
		eventHash.insert("say", eventSay);
		eventHash.insert("join", eventJoin);
		eventHash.insert("leave", eventLeave);
		eventHash.insert("game_closed", eventGameClosed);
		eventHash.insert("ready_start", eventReadyStart);
		eventHash.insert("setup_zones", eventSetupZones);
		eventHash.insert("game_start", eventGameStart);
		eventHash.insert("shuffle", eventShuffle);
		eventHash.insert("roll_die", eventRollDie);
		eventHash.insert("draw", eventDraw);
		eventHash.insert("move_card", eventMoveCard);
		eventHash.insert("create_token", eventCreateToken);
		eventHash.insert("set_card_attr", eventSetCardAttr);
		eventHash.insert("add_counter", eventAddCounter);
		eventHash.insert("set_counter", eventSetCounter);
		eventHash.insert("del_counter", eventDelCounter);
		eventHash.insert("set_active_player", eventSetActivePlayer);
		eventHash.insert("set_active_phase", eventSetActivePhase);
		eventHash.insert("dump_zone", eventDumpZone);
		eventHash.insert("stop_dump_zone", eventStopDumpZone);
	}
	
	QStringList values = line.split('|');

	IsPublic = !values.takeFirst().compare("public");
	bool ok = false;
	PlayerId = values.takeFirst().toInt(&ok);
	if (!ok)
		PlayerId = -1;
	PlayerName = values.takeFirst();
	EventType = eventHash.value(values.takeFirst(), eventInvalid);
	EventData = values;
}

QHash<QString, ChatEventType> ChatEventData::eventHash;

ChatEventData::ChatEventData(const QString &line)
{
	if (eventHash.isEmpty()) {
		eventHash.insert("list_channels", eventChatListChannels);
		eventHash.insert("join_channel", eventChatJoinChannel);
		eventHash.insert("list_players", eventChatListPlayers);
		eventHash.insert("leave_channel", eventChatLeaveChannel);
		eventHash.insert("say", eventChatSay);
		eventHash.insert("server_message", eventChatServerMessage);
	}
	
	QStringList values = line.split('|');
	values.removeFirst();
	eventType = eventHash.value(values.takeFirst(), eventChatInvalid);
	eventData = values;
}

PendingCommand::PendingCommand(int _msgid)
	: QObject(), msgid(_msgid), time(0)
{
}

void PendingCommand::responseReceived(ServerResponse resp)
{
	emit finished(resp);
	deleteLater();
}

void PendingCommand::checkTimeout()
{
	if (++time > 5)
		emit timeout();
}

void PendingCommand_ListPlayers::responseReceived(ServerResponse resp)
{
	if (resp == RespOk)
		emit playerListReceived(playerList);
	PendingCommand::responseReceived(resp);
}

void PendingCommand_ListZones::responseReceived(ServerResponse resp)
{
	if (resp == RespOk)
		emit zoneListReceived(zoneList);
	PendingCommand::responseReceived(resp);
}

void PendingCommand_DumpZone::responseReceived(ServerResponse resp)
{
	if (resp == RespOk)
		emit cardListReceived(cardList);
	PendingCommand::responseReceived(resp);
}

void PendingCommand_ListCounters::responseReceived(ServerResponse resp)
{
	if (resp == RespOk)
		emit counterListReceived(counterList);
	PendingCommand::responseReceived(resp);
}

void PendingCommand_DumpAll::responseReceived(ServerResponse resp)
{
	if (resp == RespOk) {
		emit playerListReceived(playerList);
		emit zoneListReceived(zoneList);
		emit cardListReceived(cardList);
		emit counterListReceived(counterList);
	}
	PendingCommand::responseReceived(resp);
}

Client::Client(QObject *parent)
	: QObject(parent), status(StatusDisconnected), MsgId(0)
{
	timer = new QTimer(this);
	timer->setInterval(1000);
	connect(timer, SIGNAL(timeout()), this, SLOT(ping()));

	socket = new QTcpSocket(this);
	socket->setTextModeEnabled(true);
	connect(socket, SIGNAL(connected()), this, SLOT(slotConnected()));
	connect(socket, SIGNAL(readyRead()), this, SLOT(readLine()));
	connect(socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(slotSocketError(QAbstractSocket::SocketError)));
}

Client::~Client()
{
	disconnectFromServer();
}

void Client::timeout()
{
	emit serverTimeout();
	disconnectFromServer();
}

void Client::slotSocketError(QAbstractSocket::SocketError /*error*/)
{
	emit logSocketError(socket->errorString());
	disconnectFromServer();
}

void Client::slotConnected()
{
	timer->start();
	setStatus(StatusAwaitingWelcome);
}

void Client::removePendingCommand()
{
	pendingCommands.remove(static_cast<PendingCommand *>(sender())->getMsgId());
}

void Client::loginResponse(ServerResponse response)
{
	if (response == RespOk)
		setStatus(StatusIdle);
	else {
		emit serverError(response);
		disconnectFromServer();
	}
}

void Client::enterGameResponse(ServerResponse response)
{
	if (response == RespOk)
		setStatus(StatusPlaying);
}

void Client::leaveGameResponse(ServerResponse response)
{
	if (response == RespOk)
		setStatus(StatusIdle);
}

void Client::readLine()
{
	while (socket->canReadLine()) {
		QString line = QString(socket->readLine()).trimmed();

		if (line.isNull())
			break;
		qDebug(QString("<< %1").arg(line).toLatin1());
		QStringList values = line.split("|");
		QString prefix = values.takeFirst();
		// prefix is one of {welcome, private, public, resp, list_games, list_players, list_counters, list_zones, dump_zone}
		if ((prefix == "private") || (prefix == "public")) {
			ServerEventData event(line);
			if (event.getEventType() == eventPlayerId) {
				QStringList data = event.getEventData();
				if (data.size() != 2) {
					// XXX
				}
				bool ok;
				int id = data[0].toInt(&ok);
				if (!ok) {
					// XXX
				}
				emit playerIdReceived(id, data[1]);
			} else
				emit gameEvent(event);
		} else if (prefix == "chat") {
			emit chatEvent(ChatEventData(line));
		} else if (prefix == "resp") {
			if (values.size() != 2) {
				qDebug("Client::parseCommand: Invalid response");
				continue;
			}
			bool ok;
			int msgid = values.takeFirst().toInt(&ok);
			PendingCommand *pc = pendingCommands.value(msgid, 0);
			if (!ok || !pc) {
				qDebug("Client::parseCommand: Invalid msgid");
				continue;
			}
			ServerResponse resp;
			if (values[0] == "ok")
				resp = RespOk;
			else if (values[0] == "password")
				resp = RespPassword;
			else
				resp = RespErr;
			pc->responseReceived(resp);
		} else if (prefix == "list_games") {
			if (values.size() != 8) {
				emit protocolError();
				continue;
			}
			emit gameListEvent(ServerGame(values[0].toInt(), values[5], values[1], values[2].toInt(), values[3].toInt(), values[4].toInt(), values[6].toInt(), values[7].toInt()));
		} else if (prefix == "welcome") {
			if (values.size() != 2) {
				emit protocolError();
				disconnectFromServer();
			} else if (values[0].toInt() != protocolVersion) {
				emit protocolVersionMismatch();
				disconnectFromServer();
			} else {
				emit welcomeMsgReceived(values[1]);
				setStatus(StatusLoggingIn);
				login(playerName, password);
			}
		} else if (prefix == "list_players") {
			if (values.size() != 4) {
				emit protocolError();
				continue;
			}
			int cmdid = values.takeFirst().toInt();
			PendingCommand *pc = pendingCommands.value(cmdid, 0);
			ServerPlayer sp(values[0].toInt(), values[1], values[2].toInt());
			
			PendingCommand_ListPlayers *pcLP = qobject_cast<PendingCommand_ListPlayers *>(pc);
			if (pcLP)
				pcLP->addPlayer(sp);
			else {
				PendingCommand_DumpAll *pcDA = qobject_cast<PendingCommand_DumpAll *>(pc);
				if (pcDA)
					pcDA->addPlayer(sp);
				else
					emit protocolError();
			}
		} else if (prefix == "dump_zone") {
			if (values.size() != 11) {
				emit protocolError();
				continue;
			}
			int cmdid = values.takeFirst().toInt();
			PendingCommand *pc = pendingCommands.value(cmdid, 0);
			ServerZoneCard szc(values[0].toInt(), values[1], values[2].toInt(), values[3], values[4].toInt(), values[5].toInt(), values[6].toInt(), values[7] == "1", values[8] == "1", values[9]);
	
			PendingCommand_DumpZone *pcDZ = qobject_cast<PendingCommand_DumpZone *>(pc);
			if (pcDZ)
				pcDZ->addCard(szc);
			else {
				PendingCommand_DumpAll *pcDA = qobject_cast<PendingCommand_DumpAll *>(pc);
				if (pcDA)
					pcDA->addCard(szc);
				else
					emit protocolError();
			}
		} else if (prefix == "list_zones") {
			if (values.size() != 6) {
				emit protocolError();
				continue;
			}
			int cmdid = values.takeFirst().toInt();
			PendingCommand *pc = pendingCommands.value(cmdid, 0);
			ServerZone::ZoneType type;
			if (values[2] == "private")
				type = ServerZone::PrivateZone;
			else if (values[2] == "hidden")
				type = ServerZone::HiddenZone;
			else
				type = ServerZone::PublicZone;
			ServerZone sz(values[0].toInt(), values[1], type, values[3] == "1", values[4].toInt());
			
			PendingCommand_ListZones *pcLZ = qobject_cast<PendingCommand_ListZones *>(pc);
			if (pcLZ)
				pcLZ->addZone(sz);
			else {
				PendingCommand_DumpAll *pcDA = qobject_cast<PendingCommand_DumpAll *>(pc);
				if (pcDA)
					pcDA->addZone(sz);
				else
					emit protocolError();
			}
		} else if (prefix == "list_counters") {
			if (values.size() != 6) {
				emit protocolError();
				continue;
			}
			int cmdid = values.takeFirst().toInt();
			PendingCommand *pc = pendingCommands.value(cmdid, 0);
			int colorValue = values[3].toInt();
			ServerCounter sc(values[0].toInt(), values[1].toInt(), values[2], QColor(colorValue / 65536, (colorValue % 65536) / 256, colorValue % 256), values[4].toInt(), values[5].toInt());
			
			PendingCommand_ListCounters *pcLC = qobject_cast<PendingCommand_ListCounters *>(pc);
			if (pcLC)
				pcLC->addCounter(sc);
			else {
				PendingCommand_DumpAll *pcDA = qobject_cast<PendingCommand_DumpAll *>(pc);
				if (pcDA)
					pcDA->addCounter(sc);
				else
					emit protocolError();
			}
		} else
			emit protocolError();
	}
}

void Client::setStatus(const ProtocolStatus _status)
{
	if (_status != status) {
		status = _status;
		emit statusChanged(_status);
	}
}

void Client::msg(const QString &s)
{
	qDebug(QString(">> %1").arg(s).toLatin1());
	QTextStream stream(socket);
	stream.setCodec("UTF-8");
	stream << s << endl;
	stream.flush();
}

PendingCommand *Client::cmd(const QString &s, PendingCommand *_pc)
{
	msg(QString("%1|%2").arg(++MsgId).arg(s));
	PendingCommand *pc;
	if (_pc) {
		pc = _pc;
		pc->setMsgId(MsgId);
	} else
		pc = new PendingCommand(MsgId);
	pendingCommands.insert(MsgId, pc);
	connect(pc, SIGNAL(finished(ServerResponse)), this, SLOT(removePendingCommand()));
	connect(pc, SIGNAL(timeout()), this, SLOT(timeout()));
	connect(timer, SIGNAL(timeout()), pc, SLOT(checkTimeout()));
	return pc;
}

void Client::connectToServer(const QString &hostname, unsigned int port, const QString &_playerName, const QString &_password)
{
	disconnectFromServer();
	
	playerName = _playerName;
	password = _password;
	socket->connectToHost(hostname, port);
	setStatus(StatusConnecting);
}

void Client::disconnectFromServer()
{
	timer->stop();

	QList<PendingCommand *> pc = pendingCommands.values();
	for (int i = 0; i < pc.size(); i++)
		delete pc[i];
	pendingCommands.clear();

	setStatus(StatusDisconnected);
	socket->close();
}

void Client::ping()
{
	cmd("ping");
}

PendingCommand *Client::chatListChannels()
{
	return cmd("chat_list_channels");
}

PendingCommand_ChatJoinChannel *Client::chatJoinChannel(const QString &name)
{
	return static_cast<PendingCommand_ChatJoinChannel *>(cmd(QString("chat_join_channel|%1").arg(name), new PendingCommand_ChatJoinChannel(name)));
}

PendingCommand *Client::chatLeaveChannel(const QString &name)
{
	return cmd(QString("chat_leave_channel|%1").arg(name));
}

PendingCommand *Client::chatSay(const QString &channel, const QString &s)
{
	return cmd(QString("chat_say|%1|%2").arg(channel).arg(s));
}

PendingCommand *Client::listGames()
{
	return cmd("list_games");
}

PendingCommand_ListPlayers *Client::listPlayers()
{
	return static_cast<PendingCommand_ListPlayers *>(cmd("list_players", new PendingCommand_ListPlayers));
}

PendingCommand *Client::createGame(const QString &description, const QString &password, unsigned int maxPlayers, bool spectatorsAllowed)
{
	PendingCommand *pc = cmd(QString("create_game|%1|%2|%3|%4").arg(description).arg(password).arg(maxPlayers).arg(spectatorsAllowed ? 1 : 0));
	connect(pc, SIGNAL(finished(ServerResponse)), this, SLOT(enterGameResponse(ServerResponse)));
	return pc;
}

PendingCommand *Client::joinGame(int gameId, const QString &password, bool spectator)
{
	PendingCommand *pc = cmd(QString("join_game|%1|%2|%3").arg(gameId).arg(password).arg(spectator ? 1 : 0));
	connect(pc, SIGNAL(finished(ServerResponse)), this, SLOT(enterGameResponse(ServerResponse)));
	return pc;
}

PendingCommand *Client::leaveGame()
{
	PendingCommand *pc = cmd("leave_game");
	connect(pc, SIGNAL(finished(ServerResponse)), this, SLOT(leaveGameResponse(ServerResponse)));
	return pc;
}

PendingCommand *Client::login(const QString &name, const QString &pass)
{
	PendingCommand *pc = cmd(QString("login|%1|%2").arg(name).arg(pass));
	connect(pc, SIGNAL(finished(ServerResponse)), this, SLOT(loginResponse(ServerResponse)));
	return pc;
}

PendingCommand *Client::say(const QString &s)
{
	return cmd(QString("say|%1").arg(s));
}

PendingCommand *Client::shuffle()
{
	return cmd("shuffle");
}

PendingCommand *Client::rollDie(unsigned int sides)
{
	return cmd(QString("roll_die|%1").arg(sides));
}

PendingCommand *Client::drawCards(unsigned int number)
{
	return cmd(QString("draw_cards|%1").arg(number));
}

PendingCommand *Client::moveCard(int cardid, const QString &startzone, const QString &targetzone, int x, int y, bool faceDown)
{
	// if startzone is public: cardid is the card's id
	// else: cardid is the position of the card in the zone (e.g. deck)
	return cmd(QString("move_card|%1|%2|%3|%4|%5|%6").arg(cardid).arg(startzone).arg(targetzone).arg(x).arg(y).arg(faceDown ? 1 : 0));
}

PendingCommand *Client::createToken(const QString &zone, const QString &name, const QString &powtough, int x, int y)
{
	return cmd(QString("create_token|%1|%2|%3|%4|%5").arg(zone).arg(name).arg(powtough).arg(x).arg(y));
}

PendingCommand *Client::setCardAttr(const QString &zone, int cardid, const QString &aname, const QString &avalue)
{
	return cmd(QString("set_card_attr|%1|%2|%3|%4").arg(zone).arg(cardid).arg(aname).arg(avalue));
}

void Client::submitDeck(const QStringList &deck)
{
	cmd("submit_deck");
	QStringListIterator i(deck);
	while (i.hasNext())
		msg(i.next());
	msg(".");
}

PendingCommand *Client::readyStart()
{
	return cmd("ready_start");
}

PendingCommand *Client::incCounter(int counterId, int delta)
{
	return cmd(QString("inc_counter|%1|%2").arg(counterId).arg(delta));
}

PendingCommand *Client::addCounter(const QString &counterName, QColor color, int radius, int value)
{
	return cmd(QString("add_counter|%1|%2|%3|%4").arg(counterName).arg(color.red() * 65536 + color.green() * 256 + color.blue()).arg(radius).arg(value));
}

PendingCommand *Client::setCounter(int counterId, int value)
{
	return cmd(QString("set_counter|%1|%2").arg(counterId).arg(value));
}

PendingCommand *Client::delCounter(int counterId)
{
	return cmd(QString("del_counter|%1").arg(counterId));
}

PendingCommand_ListCounters *Client::listCounters(int playerId)
{
	PendingCommand_ListCounters *pc = new PendingCommand_ListCounters(playerId);
	cmd(QString("list_counters|%1").arg(playerId), pc);
	return pc;
}

PendingCommand *Client::nextTurn()
{
	return cmd(QString("next_turn"));
}

PendingCommand *Client::setActivePhase(int phase)
{
	return cmd(QString("set_active_phase|%1").arg(phase));
}

PendingCommand_ListZones *Client::listZones(int playerId)
{
	PendingCommand_ListZones *pc = new PendingCommand_ListZones(playerId);
	cmd(QString("list_zones|%1").arg(playerId), pc);
	return pc;
}

PendingCommand_DumpZone *Client::dumpZone(int player, const QString &zone, int numberCards)
{
	PendingCommand_DumpZone *pc = new PendingCommand_DumpZone(player, zone, numberCards);
	cmd(QString("dump_zone|%1|%2|%3").arg(player).arg(zone).arg(numberCards), pc);
	return pc;
}

PendingCommand *Client::stopDumpZone(int player, const QString &zone)
{
	return cmd(QString("stop_dump_zone|%1|%2").arg(player).arg(zone));
}

PendingCommand_DumpAll *Client::dumpAll()
{
	PendingCommand_DumpAll *pc = new PendingCommand_DumpAll;
	cmd("dump_all", pc);
	return pc;
}
