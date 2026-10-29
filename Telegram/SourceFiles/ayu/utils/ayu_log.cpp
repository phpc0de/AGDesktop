#include "ayu/utils/ayu_log.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QtGlobal>

namespace Ayu::Log {
namespace {

QMutex WriteMutex;

QString Path() {
	return u"./tdata/logs/ayu-%1.log"_q.arg(
		QDateTime::currentDateTime().toString(u"yyyyMMdd"_q));
}

} // namespace

void Write(const QString &category, const QString &message) {
	QMutexLocker lock(&WriteMutex);
	QDir().mkpath(u"./tdata/logs"_q);
	auto logFile = QFile(Path());
	if (!logFile.open(QIODevice::Append | QIODevice::Text)) {
		return;
	}
	const auto line = QDateTime::currentDateTime().toString(u"HH:mm:ss.zzz"_q)
		+ u" ["_q + category + u"] "_q + message + u"\n"_q;
	logFile.write(line.toUtf8());
}

} // namespace Ayu::Log
