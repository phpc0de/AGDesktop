// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/debug/mtp_devtools.h"

#include "ayu/ui/toasts.h"
#include "ayu/utils/ayu_log.h"
#include "core/application.h"
#include "main/main_account.h"
#include "mtproto/details/mtproto_dump_to_text.h"
#include "mtproto/mtproto_response.h"
#include "rpl/event_stream.h"

#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
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
		buttons->addWidget(_copyText);
		buttons->addWidget(_copyHex);
		buttons->addWidget(_replay);
		buttons->addStretch(1);
		root->addLayout(buttons);

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
	auto entry = MtpEntry{
		.requestId = id,
		.when = crl::now(),
		.kind = 1,
	};
	const auto *from = reply.constData();
	const auto *end = from + reply.size();
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

void ReplayMtpRequest(mtpRequestId id) {
	const auto it = std::find_if(
		Entries.rbegin(),
		Entries.rend(),
		[&](const MtpEntry &entry) {
			return entry.kind == 0 && entry.requestId == id;
		});
	if (it == Entries.rend() || it->body.isEmpty()) {
		Ayu::Log::Write(
			u"mtpdev"_q,
			u"replay miss id=%1"_q.arg(id));
		return;
	}
	const auto source = *it;
	auto request = MTP::details::SerializedRequest::Prepare(
		uint32(source.body.size()));
	for (const auto prime : source.body) {
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
		source.dc,
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
