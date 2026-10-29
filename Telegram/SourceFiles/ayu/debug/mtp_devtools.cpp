// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/debug/mtp_devtools.h"

#include "ayu/ui/toasts.h"
#include "ayu/utils/ayu_log.h"
#include "base/random.h"
#include "core/application.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/details/mtproto_dump_to_text.h"
#include "mtproto/mtproto_response.h"
#include "rpl/event_stream.h"
#include "scheme.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>
#include <crl/crl.h>

namespace Ayu::Debug {
namespace {

constexpr auto kMaxEntries = 600;
constexpr auto kMaxDumpPrimes = 8192;
constexpr auto kMaxTextChars = 24000;

QString DumpPrimes(const mtpPrime *from, const mtpPrime *end) {
	if (!from || end <= from) {
		return {};
	}
	if (end - from > kMaxDumpPrimes) {
		end = from + kMaxDumpPrimes;
		auto text = MTP::details::DumpToText(from, end);
		text += u"\n... (truncated)"_q;
		return text;
	}
	return MTP::details::DumpToText(from, end);
}

QString FirstLine(const QString &text) {
	const auto line = text.section('\n', 0, 0).trimmed();
	return line.isEmpty() ? u"?"_q : line;
}

QString KindName(int kind) {
	switch (kind) {
	case 0: return u"req"_q;
	case 1: return u"res"_q;
	default: return u"err"_q;
	}
}

struct RawPreset {
	QString name;
	QString sample;
	Fn<MTP::details::SerializedRequest(
		not_null<Main::Session*>,
		const QJsonObject&)> build;
};

int IntOf(const QJsonObject &o, const QString &key, int fallback = 0) {
	return o.value(key).toVariant().toInt(fallback);
}

MTPInputPeer PeerOf(not_null<Main::Session*> session, const QJsonObject &o) {
	const auto user = o.value(u"user"_q).toVariant().toLongLong();
	const auto chat = o.value(u"chat"_q).toVariant().toLongLong();
	const auto channel = o.value(u"channel"_q).toVariant().toLongLong();
	if (user) {
		return session->data().peer(PeerId(UserId(user)))->input();
	} else if (channel) {
		return session->data().peer(PeerId(ChannelId(channel)))->input();
	} else if (chat) {
		return session->data().peer(PeerId(ChatId(chat)))->input();
	}
	Ayu::Log::Write(
		u"mtpdev"_q,
		u"raw peer json needs user/chat/channel: %1"_q.arg(
			QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact))));
	return MTP_inputPeerEmpty();
}

MTPInputChannel ChannelOf(
		not_null<Main::Session*> session,
		const QJsonObject &o) {
	const auto channel = o.value(u"channel"_q).toVariant().toLongLong();
	const auto peer = channel
		? session->data().peer(PeerId(ChannelId(channel)))
		: nullptr;
	return (peer && peer->asChannel())
		? peer->asChannel()->inputChannel()
		: MTP_inputChannelEmpty();
}

QVector<MTPint> IntVectorOf(const QJsonObject &o, const QString &key) {
	auto result = QVector<MTPint>();
	for (const auto &value : o.value(key).toArray()) {
		result.push_back(MTP_int(value.toVariant().toInt()));
	}
	return result;
}

const std::vector<RawPreset> &RawPresets() {
	static const auto kPresets = std::vector<RawPreset>{
		{
			u"messages.getHistory"_q,
			u"{\"peer\": {\"channel\": 123}, \"offset_id\": 0, \"limit\": 20}"_q,
			[](not_null<Main::Session*> session, const QJsonObject &o) {
				return MTP::details::SerializedRequest::Serialize(
					MTPmessages_GetHistory(
						PeerOf(session, o),
						MTP_int(IntOf(o, u"offset_id"_q)),
						MTP_int(0),
						MTP_int(0),
						MTP_int(IntOf(o, u"limit"_q, 20)),
						MTP_int(0),
						MTP_int(0),
						MTP_long(0)));
			},
		},
		{
			u"messages.sendMessage"_q,
			u"{\"peer\": {\"user\": 1}, \"message\": \"hello\", \"silent\": false, \"noforwards\": false}"_q,
			[](not_null<Main::Session*> session, const QJsonObject &o) {
				using Flag = MTPmessages_SendMessage::Flag;
				auto flags = MTPmessages_SendMessage::Flags(0);
				if (o.value(u"no_webpage"_q).toBool()) flags |= Flag::f_no_webpage;
				if (o.value(u"silent"_q).toBool()) flags |= Flag::f_silent;
				if (o.value(u"clear_draft"_q).toBool()) flags |= Flag::f_clear_draft;
				if (o.value(u"noforwards"_q).toBool()) flags |= Flag::f_noforwards;
				return MTP::details::SerializedRequest::Serialize(
					MTPmessages_SendMessage(
						MTP_flags(flags),
						PeerOf(session, o),
						MTPInputReplyTo(),
						MTP_string(o.value(u"message"_q).toString()),
						MTP_long(base::RandomValue<uint64>()),
						MTPReplyMarkup(),
						MTPvector<MTPMessageEntity>(),
						MTP_int(0),
						MTP_int(0),
						MTP_inputPeerEmpty(),
						MTPInputQuickReplyShortcut(),
						MTP_long(0),
						MTP_long(0),
						MTPSuggestedPost(),
						MTPInputRichMessage()));
			},
		},
		{
			u"messages.editMessage"_q,
			u"{\"peer\": {\"channel\": 123}, \"id\": 5, \"message\": \"new text\"}"_q,
			[](not_null<Main::Session*> session, const QJsonObject &o) {
				using Flag = MTPmessages_EditMessage::Flag;
				auto flags = MTPmessages_EditMessage::Flags(0);
				if (o.contains(u"message"_q)) flags |= Flag::f_message;
				return MTP::details::SerializedRequest::Serialize(
					MTPmessages_EditMessage(
						MTP_flags(flags),
						PeerOf(session, o),
						MTP_int(IntOf(o, u"id"_q)),
						MTP_string(o.value(u"message"_q).toString()),
						MTPInputMedia(),
						MTPReplyMarkup(),
						MTPvector<MTPMessageEntity>(),
						MTP_int(0),
						MTP_int(0),
						MTP_int(0),
						MTPInputRichMessage()));
			},
		},
		{
			u"messages.deleteMessages"_q,
			u"{\"revoke\": true, \"id\": [5]}"_q,
			[](not_null<Main::Session*> session, const QJsonObject &o) {
				using Flag = MTPmessages_DeleteMessages::Flag;
				auto flags = MTPmessages_DeleteMessages::Flags(0);
				if (o.value(u"revoke"_q).toBool()) flags |= Flag::f_revoke;
				return MTP::details::SerializedRequest::Serialize(
					MTPmessages_DeleteMessages(
						MTP_flags(flags),
						MTP_bool(o.value(u"revoke"_q).toBool()),
						MTP_vector<MTPint>(IntVectorOf(o, u"id"_q))));
			},
		},
		{
			u"messages.readHistory"_q,
			u"{\"peer\": {\"channel\": 123}, \"max_id\": 0}"_q,
			[](not_null<Main::Session*> session, const QJsonObject &o) {
				return MTP::details::SerializedRequest::Serialize(
					MTPmessages_ReadHistory(
						PeerOf(session, o),
						MTP_int(IntOf(o, u"max_id"_q))));
			},
		},
		{
			u"channels.getMessages"_q,
			u"{\"channel\": 123, \"id\": [5]}"_q,
			[](not_null<Main::Session*> session, const QJsonObject &o) {
				auto ids = QVector<MTPInputMessage>();
				for (const auto &value : IntVectorOf(o, u"id"_q)) {
					ids.push_back(MTP_inputMessageID(value));
				}
				return MTP::details::SerializedRequest::Serialize(
					MTPchannels_GetMessages(
						ChannelOf(session, o),
						MTP_vector<MTPInputMessage>(ids)));
			},
		},
		{
			u"channels.joinChannel"_q,
			u"{\"channel\": 123}"_q,
			[](not_null<Main::Session*> session, const QJsonObject &o) {
				return MTP::details::SerializedRequest::Serialize(
					MTPchannels_JoinChannel(ChannelOf(session, o)));
			},
		},
		{
			u"contacts.resolveUsername"_q,
			u"{\"username\": \"telegram\"}"_q,
			[](not_null<Main::Session*> session, const QJsonObject &o) {
				return MTP::details::SerializedRequest::Serialize(
					MTPcontacts_ResolveUsername(
						MTP_flags(MTPcontacts_ResolveUsername::Flags(0)),
						MTP_string(o.value(u"username"_q).toString()),
						MTPstring()));
			},
		},
	};
	return kPresets;
}

QStringList RawConsolePresetNames() {
	auto result = QStringList();
	for (const auto &preset : RawPresets()) {
		result.push_back(preset.name);
	}
	return result;
}

void SendRawConsoleRequest(const QString &preset, const QString &json) {
	const auto &presets = RawPresets();
	const auto it = std::find_if(
		presets.begin(),
		presets.end(),
		[&](const RawPreset &candidate) { return candidate.name == preset; });
	if (it == presets.end()) {
		Ayu::Log::Write(u"mtpdev"_q, u"raw console unknown preset %1"_q.arg(preset));
		return;
	}
	const auto session = Core::App().activeAccount().maybeSession();
	if (!session) {
		Ayu::Log::Write(u"mtpdev"_q, u"raw console no session"_q);
		return;
	}
	auto error = QJsonParseError{};
	const auto doc = QJsonDocument::fromJson(json.toUtf8(), &error);
	if (error.error != QJsonParseError::NoError || !doc.isObject()) {
		Ayu::Log::Write(
			u"mtpdev"_q,
			u"raw console bad json: %1"_q.arg(error.errorString()));
		return;
	}
	auto request = it->build(session, doc.object());
	const auto newId = MTP::details::GetNextRequestId();
	RecordMtpRequest(newId, request);
	auto &instance = Core::App().activeAccount().mtp();
	instance.sendSerialized(
		newId,
		std::move(request),
		MTP::ResponseHandler{
			.done = [=](const MTP::Response &response) {
				RecordMtpResponse(newId, response.reply);
				Ayu::Log::Write(
					u"mtpdev"_q,
					u"raw %1 new=%2 done"_q.arg(preset).arg(newId));
				return true;
			},
			.fail = [=](const MTP::Error &failError, const MTP::Response &) {
				RecordMtpError(newId, failError);
				Ayu::Log::Write(
					u"mtpdev"_q,
					u"raw %1 new=%2 fail %3"_q
						.arg(preset)
						.arg(newId)
						.arg(failError.type()));
				return false;
			}},
		0,
		0,
		0);
	Ayu::Log::Write(
		u"mtpdev"_q,
		u"raw %1 new=%2 sent"_q.arg(preset).arg(newId));
}

class DevtoolsWindow final : public QWidget {
public:
	DevtoolsWindow() {
		setWindowTitle(u"AyuGram MTP Devtools"_q);
		resize(960, 640);

		auto *root = new QVBoxLayout(this);
		auto *top = new QHBoxLayout();
		_filter = new QLineEdit(this);
		_filter->setPlaceholderText(u"filter (name / id / text)"_q);
		_pin = new QPushButton(u"Pin"_q, this);
		_pin->setCheckable(true);
		_clear = new QPushButton(u"Clear"_q, this);
		top->addWidget(_filter, 1);
		top->addWidget(_pin);
		top->addWidget(_clear);
		root->addLayout(top);

		auto *split = new QHBoxLayout();
		_list = new QListWidget(this);
		_list->setMinimumWidth(420);
		_detail = new QPlainTextEdit(this);
		_detail->setReadOnly(true);
		_detail->setLineWrapMode(QPlainTextEdit::NoWrap);
		split->addWidget(_list, 2);
		split->addWidget(_detail, 3);
		root->addLayout(split, 1);

		auto *buttons = new QHBoxLayout();
		_copyText = new QPushButton(u"Copy TL"_q, this);
		_copyHex = new QPushButton(u"Copy hex"_q, this);
		_replay = new QPushButton(u"Replay"_q, this);
		_editReplay = new QPushButton(u"Edit & Replay"_q, this);
		buttons->addWidget(_copyText);
		buttons->addWidget(_copyHex);
		buttons->addWidget(_replay);
		buttons->addWidget(_editReplay);
		buttons->addStretch(1);
		root->addLayout(buttons);

		auto *consoleRow = new QHBoxLayout();
		_consolePreset = new QComboBox(this);
		_consolePreset->addItems(RawConsolePresetNames());
		_consoleSend = new QPushButton(u"Send raw"_q, this);
		consoleRow->addWidget(_consolePreset, 1);
		consoleRow->addWidget(_consoleSend);
		root->addLayout(consoleRow);

		_consoleParams = new QPlainTextEdit(this);
		_consoleParams->setMaximumHeight(110);
		_consoleParams->setPlaceholderText(u"JSON params (peer: {\"user\":id} / {\"chat\":id} / {\"channel\":id})"_q);
		root->addWidget(_consoleParams);

		connect(_filter, &QLineEdit::textChanged, this, [=] {
			refresh();
		});
		connect(_pin, &QPushButton::toggled, this, [=](bool checked) {
			setWindowFlag(Qt::WindowStaysOnTopHint, checked);
			show();
		});
		connect(_clear, &QPushButton::clicked, this, [=] {
			ClearMtpEntries();
			refresh();
		});
		connect(_list, &QListWidget::currentRowChanged, this, [=](int row) {
			const auto entry = entryAt(row);
			_detail->setPlainText(entry ? entry->text : QString());
		});
		connect(_copyText, &QPushButton::clicked, this, [=] {
			QApplication::clipboard()->setText(_detail->toPlainText());
		});
		connect(_copyHex, &QPushButton::clicked, this, [=] {
			const auto entry = entryAt(_list->currentRow());
			if (!entry || entry->body.isEmpty()) {
				return;
			}
			auto parts = QStringList();
			parts.reserve(entry->body.size());
			for (const auto prime : entry->body) {
				parts << QString::number(quint32(prime), 16);
			}
			QApplication::clipboard()->setText(parts.join(u" "_q));
		});
		connect(_replay, &QPushButton::clicked, this, [=] {
			const auto entry = entryAt(_list->currentRow());
			if (!entry || entry->kind != 0) {
				return;
			}
			ReplayMtpRequest(entry->requestId);
		});
		connect(_editReplay, &QPushButton::clicked, this, [=] {
			const auto entry = entryAt(_list->currentRow());
			if (!entry || entry->kind != 0 || entry->body.isEmpty()) {
				return;
			}
			// WHY: the modal dialog lets live traffic reallocate _rows.
			const auto requestId = entry->requestId;
			auto words = QStringList();
			words.reserve(entry->body.size());
			for (const auto prime : entry->body) {
				words << u"%1"_q.arg(quint32(prime), 8, 16, QChar(u'0'));
			}
			const auto edited = QInputDialog::getMultiLineText(
				this,
				u"Edit primes (hex)"_q,
				u"32-bit prime words, space separated"_q,
				words.join(u" "_q));
			if (edited.isEmpty()) {
				return;
			}
			auto body = QVector<mtpPrime>();
			for (const auto &token : edited.simplified().split(u' ')) {
				auto ok = false;
				const auto value = token.toUInt(&ok, 16);
				if (ok) {
					body.push_back(mtpPrime(value));
				}
			}
			if (body.isEmpty()) {
				return;
			}
			ReplayMtpRequest(requestId, &body);
		});
		connect(_consolePreset, &QComboBox::currentIndexChanged, this, [=](int index) {
			const auto &presets = RawPresets();
			if (index >= 0 && index < int(presets.size())) {
				_consoleParams->setPlainText(presets[index].sample);
			}
		});
		connect(_consoleSend, &QPushButton::clicked, this, [=] {
			SendRawConsoleRequest(
				_consolePreset->currentText(),
				_consoleParams->toPlainText());
		});
		if (_consolePreset->count() > 0) {
			_consoleParams->setPlainText(RawPresets().front().sample);
		}

		MtpEntriesUpdates(
		) | rpl::on_next([=] {
			refreshSoft();
		}, _lifetime);

		refresh();
	}

private:
	[[nodiscard]] const MtpEntry *entryAt(int row) const {
		if (row < 0 || row >= int(_rows.size())) {
			return nullptr;
		}
		return &_rows[row];
	}

	void refreshSoft() {
		const auto id = _list->currentRow();
		refresh();
		if (id >= 0 && id < _list->count()) {
			_list->setCurrentRow(id);
		}
		_list->scrollToBottom();
	}

	void refresh() {
		const auto needle = _filter->text().trimmed().toLower();
		const auto entries = MtpEntriesSnapshot();
		_rows.clear();
		_list->clear();
		for (const auto &entry : entries) {
			const auto time = QDateTime::fromMSecsSinceEpoch(entry.when)
				.time().toString(u"HH:mm:ss.zzz"_q);
			const auto line = u"#%1  %2  %3  %4"_q
				.arg(entry.requestId)
				.arg(time)
				.arg(KindName(entry.kind))
				.arg(FirstLine(entry.name));
			if (!needle.isEmpty()
				&& !entry.text.toLower().contains(needle)
				&& !line.toLower().contains(needle)) {
				continue;
			}
			_rows.push_back(entry);
			_list->addItem(line);
		}
	}

	rpl::lifetime _lifetime;
	std::vector<MtpEntry> _rows;
	QLineEdit *_filter = nullptr;
	QPushButton *_pin = nullptr;
	QPushButton *_clear = nullptr;
	QListWidget *_list = nullptr;
	QPlainTextEdit *_detail = nullptr;
	QPushButton *_copyText = nullptr;
	QPushButton *_copyHex = nullptr;
	QPushButton *_replay = nullptr;
	QPushButton *_editReplay = nullptr;
	QComboBox *_consolePreset = nullptr;
	QPushButton *_consoleSend = nullptr;
	QPlainTextEdit *_consoleParams = nullptr;

};

std::vector<MtpEntry> Entries;
rpl::event_stream<> Updates;
QPointer<DevtoolsWindow> Window;

void Push(MtpEntry entry) {
	Entries.push_back(std::move(entry));
	if (Entries.size() > kMaxEntries) {
		Entries.erase(Entries.begin(), Entries.end() - kMaxEntries);
	}
	Updates.fire({});
}

} // namespace

std::vector<MtpEntry> MtpEntriesSnapshot() {
	return Entries;
}

rpl::producer<> MtpEntriesUpdates() {
	return Updates.events();
}

void RecordMtpRequest(
		mtpRequestId id,
		const MTP::details::SerializedRequest &request) {
	if (!request || id <= 0) {
		return;
	}
	const auto &buffer = *request;
	if (buffer.size() <= MTP::details::SerializedRequest::kMessageBodyPosition) {
		return;
	}
	auto entry = MtpEntry{
		.requestId = id,
		.when = crl::now(),
		.kind = 0,
		.dc = 0,
	};
	const auto count = buffer.size()
		- MTP::details::SerializedRequest::kMessageBodyPosition;
	entry.body.reserve(count);
	const auto *first = buffer.constData()
		+ MTP::details::SerializedRequest::kMessageBodyPosition;
	for (int i = 0; i < count; ++i) {
		entry.body.push_back(first[i]);
	}
	const auto *from = entry.body.constData();
	const auto *end = from + entry.body.size();
	entry.text = DumpPrimes(from, end);
	entry.name = FirstLine(entry.text);
	Push(std::move(entry));
}

void RecordMtpResponse(mtpRequestId id, const mtpBuffer &reply) {
	const auto *from = reply.constData();
	RecordMtpResponse(id, from, from + reply.size());
}

void RecordMtpResponse(
		mtpRequestId id,
		const mtpPrime *from,
		const mtpPrime *end) {
	auto entry = MtpEntry{
		.requestId = id,
		.when = crl::now(),
		.kind = 1,
	};
	entry.text = DumpPrimes(from, end);
	entry.name = FirstLine(entry.text);
	Push(std::move(entry));
}

void RecordMtpError(mtpRequestId id, const MTP::Error &error) {
	auto entry = MtpEntry{
		.requestId = id,
		.when = crl::now(),
		.kind = 2,
	};
	entry.name = error.type();
	entry.text = u"code=%1\ntype=%2\ndescription=%3"_q
		.arg(error.code())
		.arg(error.type())
		.arg(error.description());
	Push(std::move(entry));
}

void ReplayMtpRequest(
		mtpRequestId id,
		const QVector<mtpPrime> *overrideBody) {
	const auto it = std::find_if(
		Entries.rbegin(),
		Entries.rend(),
		[&](const MtpEntry &entry) {
			return entry.kind == 0 && entry.requestId == id;
		});
	if (it == Entries.rend()) {
		Ayu::Log::Write(
			u"mtpdev"_q,
			u"replay miss id=%1"_q.arg(id));
		return;
	}
	const auto &body = overrideBody ? *overrideBody : it->body;
	if (body.isEmpty()) {
		Ayu::Log::Write(
			u"mtpdev"_q,
			u"replay empty body id=%1"_q.arg(id));
		return;
	}
	auto request = MTP::details::SerializedRequest::Prepare(
		uint32(body.size()));
	for (const auto prime : body) {
		(*request).push_back(prime);
	}
	const auto newId = MTP::details::GetNextRequestId();
	RecordMtpRequest(newId, request);
	auto &instance = Core::App().activeAccount().mtp();
	instance.sendSerialized(
		newId,
		std::move(request),
		MTP::ResponseHandler{
			.done = [=](const MTP::Response &response) {
				RecordMtpResponse(newId, response.reply);
				Ayu::Log::Write(
					u"mtpdev"_q,
					u"replay src=%1 new=%2 done"_q.arg(id).arg(newId));
				return true;
			},
			.fail = [=](const MTP::Error &error, const MTP::Response &) {
				RecordMtpError(newId, error);
				Ayu::Log::Write(
					u"mtpdev"_q,
					u"replay src=%1 new=%2 fail type=%3"_q
						.arg(id)
						.arg(newId)
						.arg(error.type()));
				return false;
			}},
		it->dc,
		0,
		0);
	Ayu::Log::Write(
		u"mtpdev"_q,
		u"replay src=%1 new=%2 sent"_q.arg(id).arg(newId));
}

void ClearMtpEntries() {
	Entries.clear();
}

void ShowMtpDevtools() {
	if (!Window) {
		Window = new DevtoolsWindow();
	}
	Window->show();
	Window->raise();
	Window->activateWindow();
}

} // namespace Ayu::Debug
