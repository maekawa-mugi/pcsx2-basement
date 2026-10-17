// SPDX-License-Identifier: GPL-3.0+
#include "AgentConsoleServer.h"
#include "QtHost.h"
#include "pcsx2/AgentConsole.h"
#include "pcsx2/VMManager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QtNetwork/QNetworkProxy>
#include <QSaveFile>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <array>
#include <memory>

namespace AgentConsoleServer
{
	static const std::array<const char*, AgentConsole::CHANNEL_COUNT> NAMES = {"ee.sio", "iop.stdout", "iop.hle"};
	static const std::array<const char*, AgentConsole::CHANNEL_COUNT> FILES = {"serial.ee-sio.bin", "serial.iop-stdout.bin", "serial.iop-hle.bin"};
	static QString Number(u64 value) { return QString::number(static_cast<qulonglong>(value)); }
	static bool SaveJSON(const QString& path, const QJsonObject& object)
	{
		QSaveFile file(path);
		if (!file.open(QIODevice::WriteOnly))
			return false;
		const auto bytes = QJsonDocument(object).toJson();
		return file.write(bytes) == bytes.size() && file.commit();
	}
	static QJsonObject InputJSON(const AgentConsole::InputResult& input)
	{
		return {{"request_id", QString::fromStdString(input.id)}, {"input_state", QString::fromStdString(input.state)},
			{"epoch", Number(input.epoch)}, {"output_offset", Number(input.output_offset)},
			{"consumed_before", Number(input.consumed_before)}, {"at_event", Number(input.at_event)}, {"size", static_cast<int>(input.size)}};
	}

	class Server final : public QObject
	{
	public:
		QString directory;
		QString run_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
		QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
		QTcpServer listener;
		QTimer timer;
		QFile events;
		QFile input_file;
		QHash<QString, QString> input_states;
		QHash<QString, QJsonObject> action_results;
		QHash<QString, QJsonObject> action_requests;
		std::array<QFile, AgentConsole::CHANNEL_COUNT> files;
		std::array<u64, AgentConsole::CHANNEL_COUNT> saved{};
		u64 last_epoch = 0;
		int ticks = 0;
		bool stopping = false;
		bool effective_saved = false;

		static QString Hash(const QByteArray& bytes)
		{
			return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
		}
		void SaveSnapshot()
		{
			const auto action = AgentConsole::GetAction();
			const auto id = QString::fromStdString(action.id);
			if (action.state != "done" || action_results.contains(id))
				return;
			const auto snapshot = AgentConsole::GetSnapshot();
			if (!snapshot || snapshot->id != action.id)
			{
				AgentConsole::SetFault("snapshot_identity_mismatch");
				return;
			}
			// The CPU may have reached the boundary after Drain() swapped buffers.
			// Once the completed snapshot is acquired, it is paused: drain again so
			// its output fingerprint cannot depend on the GUI timer's scheduling.
			DrainOutput();
			if (saved != AgentConsole::GetStatus().produced)
			{
				AgentConsole::SetFault("snapshot_output_incomplete");
				return;
			}
			QJsonObject registers;
			for (const auto& bank : snapshot->banks)
			{
				QJsonObject values;
				for (const auto& reg : bank.values)
				{
					QString value = QString::number(reg.low, 16).rightJustified(reg.bits > 64 ? 16 : reg.bits / 4, '0');
					if (reg.bits > 64)
						value.prepend(QString::number(reg.high, 16).rightJustified(16, '0'));
					values.insert(QString::fromStdString(reg.name), QJsonObject{{"bits", static_cast<int>(reg.bits)}, {"hex", value}});
				}
				registers.insert(QString::fromStdString(bank.name), values);
			}
			const auto stem = "snapshot-" + Hash(id.toUtf8());
			QJsonObject memory;
			QJsonObject raw_register_files;
			for (const auto& region : snapshot->memory)
			{
				const auto bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(region.bytes.data()), region.bytes.size());
				memory.insert(QString::fromStdString(region.name), QJsonObject{{"size", Number(region.bytes.size())}, {"sha256", Hash(bytes)}});
				// Read the frozen shadows, never MMIO handlers with read side effects.
				if (region.name == "ee.mmio" || region.name == "iop.mmio" || region.name == "ee.sio.rx")
				{
					const auto filename = stem + "-" + QString::fromStdString(region.name) + ".bin";
					QSaveFile file(directory + "/" + filename);
					if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
					{
						AgentConsole::SetFault("snapshot_register_file_failed");
						return;
					}
					raw_register_files.insert(QString::fromStdString(region.name), filename);
				}
			}
			const auto settings = QByteArray::fromStdString(snapshot->settings);
			auto canonical_settings = settings;
			canonical_settings.replace(QDir::toNativeSeparators(directory).toUtf8(), "$RUN");
			canonical_settings.replace(directory.toUtf8(), "$RUN");
			QJsonObject canonical{{"event", Number(snapshot->event)}, {"ee_cycle", Number(snapshot->ee_cycle)},
				{"iop_cycle", Number(snapshot->iop_cycle)}, {"registers", registers}, {"memory", memory}, {"settings_sha256", Hash(canonical_settings)}};
			QJsonObject output;
			for (size_t i = 0; i < files.size(); i++)
			{
				if (!files[i].seek(0))
				{
					AgentConsole::SetFault("snapshot_output_seek_failed");
					return;
				}
				const auto bytes = files[i].read(saved[i]);
				if (static_cast<u64>(bytes.size()) != saved[i])
				{
					AgentConsole::SetFault("snapshot_output_read_failed");
					return;
				}
				output.insert(NAMES[i], QJsonObject{{"size", Number(saved[i])}, {"sha256", Hash(bytes)}});
			}
			canonical.insert("output", output);
			const auto fingerprint = Hash(QJsonDocument(canonical).toJson(QJsonDocument::Compact));
			const auto filename = stem + ".json";
			auto document = canonical;
			document.insert("request_id", id);
			document.insert("run_id", run_id);
			document.insert("epoch", Number(snapshot->epoch));
			document.insert("state_sha256", fingerprint);
			document.insert("raw_register_files", raw_register_files);
			document.insert("consistency", "CPU execution exited; VU/GS synchronized; VM remains paused");
			document.insert("scope", "EE/IOP architectural registers, VU0/VU1, TLB, GS private registers and memory-region hashes; not a restorable savestate");
			if (!SaveJSON(directory + "/" + filename, document))
			{
				AgentConsole::SetFault("snapshot_save_failed");
				return;
			}
			QSaveFile config(directory + "/" + stem + ".ini");
			if (!config.open(QIODevice::WriteOnly) || config.write(settings) != settings.size() || !config.commit())
			{
				AgentConsole::SetFault("snapshot_settings_save_failed");
				return;
			}
			QJsonObject result{{"ok", true}, {"request_id", id}, {"action_state", "done"}, {"event", Number(snapshot->event)},
				{"state_sha256", fingerprint}, {"snapshot_file", filename}, {"settings_file", stem + ".ini"}, {"epoch", Number(snapshot->epoch)}};
			action_results.insert(id, result);
			Event({{"event", "checkpoint"}, {"checkpoint", result}});
		}

		bool Initialize(const QString& path, QString* error)
		{
			directory = QDir(path).absolutePath();
			if (!QDir().mkpath(directory) || QFile::exists(directory + "/run.json"))
			{
				*error = "Cannot create run directory, or run.json already exists; use a fresh directory.";
				return false;
			}
			for (size_t i = 0; i < files.size(); i++)
			{
				files[i].setFileName(directory + "/" + FILES[i]);
				if (!files[i].open(QIODevice::ReadWrite | QIODevice::NewOnly))
				{
					*error = files[i].errorString();
					return false;
				}
			}
			events.setFileName(directory + "/events.jsonl");
			if (!events.open(QIODevice::WriteOnly | QIODevice::NewOnly))
			{
				*error = events.errorString();
				return false;
			}
			input_file.setFileName(directory + "/serial.ee-sio.rx.bin");
			if (!input_file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
			{
				*error = input_file.errorString();
				return false;
			}
			listener.setProxy(QNetworkProxy::NoProxy);
			if (!listener.listen(QHostAddress::LocalHost, 0))
			{
				*error = listener.errorString();
				return false;
			}
			connect(&listener, &QTcpServer::newConnection, this, [this] {
				while (auto* socket = listener.nextPendingConnection())
					Accept(socket);
			});
			AgentConsole::Enable();
			connect(&timer, &QTimer::timeout, this, [this] { Drain(); });
			timer.start(10);
			QJsonObject manifest{{"protocol", 1}, {"run_id", run_id}, {"token", token}, {"host", "127.0.0.1"}, {"deterministic", AgentConsole::IsDeterministic()},
				{"port", listener.serverPort()}, {"pid", Number(QCoreApplication::applicationPid())},
				{"arguments", QJsonArray::fromStringList(QCoreApplication::arguments())}};
			if (!SaveJSON(directory + "/run.json", manifest))
			{
				*error = "Cannot save run.json";
				AgentConsole::Disable();
				return false;
			}
			QFile::setPermissions(directory + "/run.json", QFileDevice::ReadOwner | QFileDevice::WriteOwner);
			Event({{"event", "transport_ready"}, {"run_id", run_id}});
			return true;
		}
		void Event(const QJsonObject& object)
		{
			const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
			if (events.write(bytes) != bytes.size() || !events.flush())
				AgentConsole::SetFault("event_save_failed");
		}
		void DrainOutput()
		{
			auto output = AgentConsole::Drain();
			for (size_t i = 0; i < files.size(); i++)
			{
				if (output[i].empty())
					continue;
				if (saved[i] + output[i].size() > 256 * 1024 * 1024ULL)
				{
					AgentConsole::SetFault("archive_size_limit");
					continue;
				}
				if (!files[i].seek(saved[i]) || files[i].write(reinterpret_cast<const char*>(output[i].data()), output[i].size()) != static_cast<qint64>(output[i].size()) || !files[i].flush())
					AgentConsole::SetFault("raw_save_failed");
				else
					saved[i] += output[i].size();
			}
		}
		void Drain()
		{
			DrainOutput();
			const auto state = AgentConsole::GetStatus();
			for (auto it = input_states.begin(); it != input_states.end(); ++it)
			{
				if (it.value() != "queued")
					continue;
				const auto input = AgentConsole::GetInput(it.key().toStdString());
				if (input.state != "queued")
				{
					it.value() = QString::fromStdString(input.state);
					Event({{"event", "input_result"}, {"input", InputJSON(input)}});
				}
			}
			SaveSnapshot();
			if (!effective_saved && VMManager::GetState() == VMState::Running && state.polls > 0)
			{
				effective_saved = SaveJSON(directory + "/effective-config.json", QJsonObject{
																					 {"scope", AgentConsole::IsDeterministic() ? "isolated ELF; complete core settings in snapshot-*.ini" : "console MVP; other emulator settings are not frozen"},
																					 {"full_tlb", state.full_tlb}, {"ee_recompiler", state.ee_recompiler}, {"extra_memory", state.extra_memory},
																					 {"display_conversion", false}, {"arguments", QJsonArray::fromStringList(QCoreApplication::arguments())}});
				if (!effective_saved)
					AgentConsole::SetFault("effective_config_save_failed");
			}
			if (last_epoch != state.epoch)
			{
				last_epoch = state.epoch;
				Event({{"event", "epoch_changed"}, {"epoch", Number(last_epoch)}});
			}
			if (++ticks % 25 == 0 && !SaveJSON(directory + "/status.json", Status()))
				AgentConsole::SetFault("status_save_failed");
		}
		QJsonObject Status()
		{
			const auto state = AgentConsole::GetStatus();
			const auto vm = VMManager::GetState();
			const char* vm_name = "transitioning";
			if (vm == VMState::Running)
				vm_name = "running";
			else if (vm == VMState::Paused)
				vm_name = "paused";
			else if (vm == VMState::Shutdown)
				vm_name = "shutdown";
			else if (vm == VMState::Initializing)
				vm_name = "initializing";
			else if (vm == VMState::Stopping)
				vm_name = "stopping";
			QJsonObject channels;
			for (size_t i = 0; i < files.size(); i++)
				channels.insert(NAMES[i], QJsonObject{{"produced", Number(state.produced[i])}, {"saved", Number(saved[i])}, {"epoch_start", Number(state.epoch_start[i])}, {"file", FILES[i]}});
			return {{"ok", true}, {"run_id", run_id}, {"epoch", Number(state.epoch)}, {"vm_state", vm_name},
				{"observation", state.fault.empty() ? "ready" : "fault"}, {"fault", QString::fromStdString(state.fault)},
				{"startup_error", QString::fromStdString(state.startup_error)}, {"cpu_polls", Number(state.polls)},
				{"pc", QString::number(state.pc, 16)}, {"cycle", Number(state.cycle)}, {"rx_consumed", Number(state.rx_consumed)},
				{"guest_console_verified", false}, {"channels", channels}, {"event", Number(state.event)},
				{"deterministic", AgentConsole::IsDeterministic()}, {"determinism_profile", AgentConsole::IsDeterministic() ? "isolated-elf-v1-same-build" : "disabled"}};
		}
		void Accept(QTcpSocket* socket)
		{
			if (findChildren<QTcpSocket*>().size() >= 32)
			{
				socket->abort();
				socket->deleteLater();
				return;
			}
			socket->setParent(this);
			socket->setReadBufferSize(8192);
			auto buffer = std::make_shared<QByteArray>();
			connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
			QTimer::singleShot(15000, socket, [socket] { socket->abort(); socket->deleteLater(); });
			connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
				*buffer += socket->readAll();
				const auto newline = buffer->indexOf('\n');
				if (buffer->size() > 8192)
				{
					socket->abort();
					return;
				}
				if (newline < 0)
					return;
				QJsonParseError error;
				const auto doc = QJsonDocument::fromJson(buffer->left(newline), &error);
				if (error.error != QJsonParseError::NoError || !doc.isObject())
				{
					Reply(socket, {{"ok", false}, {"error", "invalid_json"}});
					return;
				}
				const auto request = doc.object();
				const int size = request.value("payload_size").toInt(-1);
				if (size < 0 || size > 1024)
				{
					Reply(socket, {{"ok", false}, {"error", "invalid_payload_size"}});
					return;
				}
				if (buffer->size() < newline + 1 + size)
					return;
				QObject::disconnect(socket, nullptr, this, nullptr);
				Handle(socket, request, buffer->mid(newline + 1, size));
			});
		}
		void Reply(QTcpSocket* socket, QJsonObject response, const QByteArray& bytes = {})
		{
			response.insert("payload_size", bytes.size());
			response.insert("run_id", run_id);
			socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
			if (!bytes.isEmpty())
				socket->write(bytes);
			socket->disconnectFromHost();
		}
		void Handle(QTcpSocket* socket, const QJsonObject& request, const QByteArray& payload)
		{
			if (request.value("token").toString() != token || request.value("run_id").toString() != run_id)
			{
				Reply(socket, {{"ok", false}, {"error", "session_mismatch"}});
				return;
			}
			Drain();
			const QString command = request.value("command").toString();
			if (command == "status")
			{
				Reply(socket, Status());
				return;
			}
			if (command == "stop")
			{
				AgentConsole::RequestStop();
				if (VMManager::GetState() != VMState::Running)
					g_emu_thread->shutdownVM(false);
				Reply(socket, {{"ok", true}, {"state", "stop_requested"}});
				if (VMManager::GetState() == VMState::Shutdown)
					QTimer::singleShot(100, QCoreApplication::instance(), &QCoreApplication::quit);
				return;
			}
			const auto state = AgentConsole::GetStatus();
			if (request.value("epoch").toString() != Number(state.epoch))
			{
				Reply(socket, {{"ok", false}, {"error", "epoch_mismatch"}});
				return;
			}
			if (!state.fault.empty())
			{
				Reply(socket, {{"ok", false}, {"error", "observation_fault"}, {"fault", QString::fromStdString(state.fault)}});
				return;
			}
			if (command == "snapshot" || command == "run-events" || command == "action-status")
			{
				const auto id = request.value("request_id").toString();
				if (id.isEmpty() || id.size() > 128)
				{
					Reply(socket, {{"ok", false}, {"error", "invalid_request_id"}});
					return;
				}
				if (command == "action-status")
				{
					if (action_results.contains(id))
					{
						Reply(socket, action_results.value(id));
						return;
					}
					const auto action = AgentConsole::GetAction();
					Reply(socket, {{"ok", action.id == id.toStdString()}, {"request_id", id}, {"action_state", QString::fromStdString(action.state)},
									  {"error", action.id == id.toStdString() ? "" : "unknown_action"}});
					return;
				}
				bool valid = true;
				const auto target = command == "run-events" ? request.value("until").toString().toULongLong(&valid) : 0;
				if (!valid)
				{
					Reply(socket, {{"ok", false}, {"error", "invalid_target"}});
					return;
				}
				QJsonObject specification{{"command", command}, {"until", Number(target)}};
				if (action_requests.contains(id))
				{
					if (action_requests.value(id) != specification)
					{
						Reply(socket, {{"ok", false}, {"error", "request_id_conflict"}});
						return;
					}
					if (action_results.contains(id))
					{
						Reply(socket, action_results.value(id));
						return;
					}
				}
				if (action_requests.size() >= 4096)
				{
					Reply(socket, {{"ok", false}, {"error", "action_limit"}});
					return;
				}
				const auto action = AgentConsole::RequestAction(id.toStdString(), command == "run-events" ? "run" : "snapshot", target);
				if (action.state != "queued" && action.state != "running" && action.state != "done")
				{
					Reply(socket, {{"ok", false}, {"error", QString::fromStdString(action.state)}});
					return;
				}
				if (!action_requests.contains(id))
				{
					action_requests.insert(id, specification);
					Event({{"event", "action_requested"}, {"request_id", id}, {"action", specification}, {"at_event", Number(state.event)}});
				}
				Host::RunOnCPUThread([] { AgentConsole::Poll(); });
				Reply(socket, {{"ok", true}, {"request_id", id}, {"action_state", QString::fromStdString(action.state)}});
				return;
			}
			if (command == "send" || command == "input-status")
			{
				const auto id = request.value("request_id").toString();
				if (id.isEmpty() || id.size() > 128)
				{
					Reply(socket, {{"ok", false}, {"error", "invalid_request_id"}});
					return;
				}
				const auto previous = AgentConsole::GetInput(id.toStdString());
				const auto result = command == "send" ? AgentConsole::Submit(id.toStdString(), state.epoch,
															{reinterpret_cast<const u8*>(payload.data()), static_cast<size_t>(payload.size())}) :
				                                        AgentConsole::GetInput(id.toStdString());
				if (result.state == "queued" && VMManager::GetState() == VMState::Paused)
					Host::RunOnCPUThread([] { AgentConsole::Poll(); });
				auto response = InputJSON(result);
				if (previous.state == "unknown_request" && command == "send" && result.state == "queued")
				{
					response.insert("input_file_offset", Number(input_file.pos()));
					if (input_file.write(payload) != payload.size() || !input_file.flush())
						AgentConsole::SetFault("input_save_failed");
				}
				const QString input_state = QString::fromStdString(result.state);
				if (result.state != "unknown_request" && input_states.size() < 4096 && input_states.value(id) != input_state)
				{
					Event({{"event", "input_result"}, {"input", response}});
					input_states.insert(id, input_state);
				}
				response.insert("ok", result.state == "accepted" || result.state == "queued");
				Reply(socket, response);
				return;
			}
			if (command == "read")
			{
				const auto name = request.value("channel").toString();
				size_t index = 0;
				while (index < NAMES.size() && name != NAMES[index])
					index++;
				bool valid_offset;
				const auto offset = request.value("offset").toString().toULongLong(&valid_offset);
				const auto limit = request.value("limit").toInt();
				if (index == NAMES.size() || !valid_offset || limit < 1 || limit > 65536 || offset > saved[index] || offset < state.epoch_start[index])
				{
					Reply(socket, {{"ok", false}, {"error", "invalid_read_range_or_channel"}});
					return;
				}
				if (!files[index].seek(offset))
				{
					AgentConsole::SetFault("archive_seek_failed");
					Reply(socket, {{"ok", false}, {"error", "archive_seek_failed"}});
					return;
				}
				const auto count = std::min<u64>(limit, saved[index] - offset);
				const auto bytes = files[index].read(count);
				if (static_cast<u64>(bytes.size()) != count)
				{
					AgentConsole::SetFault("archive_read_failed");
					Reply(socket, {{"ok", false}, {"error", "archive_read_failed"}});
					return;
				}
				Reply(socket, {{"ok", true}, {"epoch", Number(state.epoch)}, {"channel", name}, {"offset", Number(offset)}, {"next_offset", Number(offset + count)}, {"saved", Number(saved[index])}}, bytes);
				return;
			}
			Reply(socket, {{"ok", false}, {"error", "unknown_command"}});
		}
		void Finish()
		{
			if (stopping)
				return;
			stopping = true;
			timer.stop();
			listener.close();
			AgentConsole::Disable();
			Drain();
			auto result = Status();
			const auto state = AgentConsole::GetStatus();
			result.insert("outcome", "unknown");
			result.insert("reason", !state.startup_error.empty() ? "startup_failed" : (!state.fault.empty() ? "observation_fault" : "vm_stopped"));
			Event({{"event", "finished"}, {"result", result}});
			const auto final_state = AgentConsole::GetStatus();
			if (!final_state.fault.empty())
			{
				result.insert("observation", "fault");
				result.insert("fault", QString::fromStdString(final_state.fault));
				result.insert("reason", "observation_fault");
			}
			if (!SaveJSON(directory + "/result.json", result))
				AgentConsole::SetFault("result_save_failed");
		}
	};
	static std::unique_ptr<Server> s_server;
	bool Start(const QString& directory, QString* error)
	{
		auto server = std::make_unique<Server>();
		if (!server->Initialize(directory, error))
			return false;
		s_server = std::move(server);
		return true;
	}
	void Stop()
	{
		if (s_server)
		{
			s_server->Finish();
			s_server.reset();
		}
	}
	void ReportStartupError(const QString& error)
	{
		if (AgentConsole::IsEnabled())
			AgentConsole::SetStartupError(error.toStdString());
	}
} // namespace AgentConsoleServer
