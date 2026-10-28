#pragma once

#include "ui/toast/toast.h"

namespace Ayu::Ui {

void ShowToastWithAction(
	::Ui::Toast::Config &&config,
	const QString &buttonText,
	Fn<void()> callback);

void ShowCopyableDetails(const QString &title, const QString &details);

}
