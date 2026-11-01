// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/settings/settings_sexcat.h"

#include "lang_auto.h"
#include "ayu/ayu_settings.h"
#include "ayu/sexcat/sexcat_history_window.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

namespace Settings {

namespace {

void BuildSexCatSettings(SectionBuilder &builder) {
	auto *settings = &AyuSettings::getInstance();

	builder.addSkip();

	const auto enabledButton = builder.addButton({
		.id = u"ayu/sexcatEnabled"_q,
		.title = tr::lng_sexcat_settings_toggle(),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(settings->sexcatEnabled()),
	});
	if (enabledButton) {
		enabledButton->toggledValue(
		) | rpl::filter([=](bool enabled) {
			return (enabled != settings->sexcatEnabled());
		}) | rpl::on_next([=](bool enabled) {
			AyuSettings::getInstance().setSexcatEnabled(enabled);
		}, enabledButton->lifetime());
	}

	builder.addButton({
		.id = u"ayu/sexcatOpenHistory"_q,
		.title = tr::lng_sexcat_view_history(),
		.st = &st::settingsButtonNoIcon,
		.onClick = [] {
			SexCat::OpenHistoryWindow();
		},
	});
}

const auto kMeta = BuildHelper({
	.id = AyuSexCat::Id(),
	.parentId = AyuMain::Id(),
	.title = u"SexCat"_q,
	.icon = &st::menuIconDownload,
}, [](SectionBuilder &builder) {
	BuildSexCatSettings(builder);
});

} // namespace

rpl::producer<QString> AyuSexCat::title() {
	return rpl::single(u"SexCat"_q);
}

AyuSexCat::AyuSexCat(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

void AyuSexCat::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type AyuSexCatId() {
	return AyuSexCat::Id();
}

} // namespace Settings
