/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "menu/menu_item_download_files.h"

#include "base/base_file_utilities.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_click_handler.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history_inner_widget.h"
#include "history/history_item.h"
#include "history/view/history_view_list_widget.h" // HistoryView::SelectedItem.
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "storage/storage_account.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

// AyuGram includes
#include "ayu/sexcat/sexcat_save_hook.h"
#include "base/unique_qptr.h"
#include "history/history.h"
#include "ui/text/format_values.h"
#include "ui/widgets/menu/menu_action.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include <QMenu>


namespace Menu {
namespace {

using Documents = std::vector<std::pair<not_null<DocumentData*>, FullMsgId>>;
using Photos = std::vector<std::pair<not_null<PhotoData*>, FullMsgId>>;

[[nodiscard]] bool Collected(
		HistoryItem *item,
		Documents &documents,
		Photos &photos) {
	if (!item) {
		return false;
	} else if (item->forbidsSaving()) {
		return true;
	} else if (const auto media = item->media()) {
		if (const auto photo = media->photo()) {
			photos.emplace_back(photo, item->fullId());
			return true;
		} else if (const auto document = media->document()) {
			documents.emplace_back(document, item->fullId());
			return true;
		}
	}
	return false;
}

// One folder dialog for the whole batch when forced or configured, silent save otherwise.
void ChooseFolderAndSave(
		base::weak_ptr<Window::SessionController> weak,
		Fn<void(const QString &folderPath)> save,
		bool forceFolderDialog) {
	const auto controller = weak.get();
	if (!controller) {
		return;
	}
	if (forceFolderDialog || Core::App().settings().askDownloadPath()) {
		const auto initialPath = [] {
			const auto path = Core::App().settings().downloadPath();
			if (!path.isEmpty() && path != FileDialog::Tmp()) {
				return path.left(path.size()
					- (path.endsWith('/') ? 1 : 0));
			}
			return QString();
		}();
		const auto handleFolder = [=](const QString &result) {
			if (!result.isEmpty()) {
				const auto folderPath = result.endsWith('/')
					? result
					: (result + '/');
				save(folderPath);
			}
		};
		FileDialog::GetFolder(
			controller->window().widget().get(),
			tr::lng_download_path_choose(tr::now),
			initialPath,
			handleFolder);
	} else {
		save(QString());
	}
}

void AddAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		const Documents &documents,
		const Photos &photos,
		Fn<void()> callback,
		QString overrideText = QString(),
		const style::icon *overrideIcon = nullptr,
		bool forceFolderDialog = false) {
	const auto text = overrideText.isEmpty()
		? (documents.empty()
			? tr::lng_context_save_images_selected(tr::now)
			: tr::lng_context_save_documents_selected(tr::now))
		: overrideText;
	const auto icon = overrideIcon
		? overrideIcon
		: documents.empty()
		? &st::menuIconSaveImage
		: &st::menuIconDownload;
	const auto shouldShowToast = documents.empty();

	// Q52.4: every file of a batch download carries the same sow tag.
	auto batchTag = QString();
	for (const auto &[document, fullId] : documents) {
		if (const auto item = controller->session().data().message(fullId)) {
			batchTag = SexCat::SourceTagForItem(item);
			if (!batchTag.isEmpty()) {
				break;
			}
		}
	}
	if (batchTag.isEmpty()) {
		for (const auto &[photo, fullId] : photos) {
			if (const auto item = controller->session().data().message(fullId)) {
				batchTag = SexCat::SourceTagForItem(item);
				if (!batchTag.isEmpty()) {
					break;
				}
			}
		}
	}

	const auto weak = base::make_weak(controller);
	const auto saveImages = [=](const QString &folderPath) {
		const auto controller = weak.get();
		if (!controller) {
			return;
		}
		const auto session = &controller->session();
		const auto downloadPath = folderPath.isEmpty()
			? Core::App().settings().downloadPath()
			: folderPath;
		const auto path = downloadPath.isEmpty()
			? File::DefaultDownloadPath(session)
			: (downloadPath == FileDialog::Tmp())
			? session->local().tempDirectory()
			: downloadPath;
		if (path.isEmpty()) {
			return;
		}
		QDir().mkpath(path);

		const auto showToast = !shouldShowToast
			? Fn<void(const QString &)>(nullptr)
			: [=](const QString &lastPath) {
				const auto filter = [lastPath](const auto ...) {
					File::ShowInFolder(lastPath);
					return false;
				};
				controller->showToast({
					.text = (photos.size() > 1
							? tr::lng_mediaview_saved_images_to
							: tr::lng_mediaview_saved_to)(
						tr::now,
						lt_downloads,
						tr::link(
							tr::lng_mediaview_downloads(tr::now),
							"internal:show_saved_message"),
						tr::marked),
					.filter = filter,
					.iconLottie = u"toast/save_to_gallery"_q,
					.iconLottieSize = st::toastLottieIconSize,
					.st = &st::defaultToast,
				});
			};

		auto views = std::vector<std::shared_ptr<Data::PhotoMedia>>();
		auto dates = std::vector<TimeId>();
		for (const auto &[photo, fullId] : photos) {
			if (const auto view = photo->createMediaView()) {
				view->wanted(Data::PhotoSize::Large, fullId);
				views.push_back(view);
				const auto photoDate = photo->date();
				const auto item = session->data().message(fullId);
				dates.push_back(photoDate
					? photoDate
					: (item ? item->date() : TimeId(0)));
			}
		}

		const auto finalCheck = [=] {
			for (const auto &[photo, _] : photos) {
				if (photo->loading()) {
					return false;
				}
			}
			return true;
		};

		const auto saveToFiles = [=] {
			const auto fullPath = [&](int i) {
				return filedialogDefaultName(
					u"photo_"_q + QString::number(i),
					u".jpg"_q,
					path);
			};
			auto lastPath = QString();
			for (auto i = 0; i < views.size(); i++) {
				lastPath = fullPath(i + 1);
				if (views[i]->saveToFile(lastPath)) {
					if (dates[i] > 0) {
						auto f = QFile(lastPath);
						if (f.open(QIODevice::ReadWrite)) {
							const auto when = base::unixtime::parse(dates[i]);
							f.setFileTime(
								when,
								QFileDevice::FileModificationTime);
							f.setFileTime(
								when,
								QFileDevice::FileAccessTime);
						}
					}
					if (folderPath.isEmpty()) {
						continue;
					}
					SexCat::RecordPhotoSaveAttempt(
						lastPath,
						photos[i].first,
						photos[i].second,
						0,
						batchTag);
				}
			}
			if (showToast) {
				showToast(lastPath);
			}
		};

		if (finalCheck()) {
			saveToFiles();
		} else {
			auto lifetime = std::make_shared<rpl::lifetime>();
			session->downloaderTaskFinished(
			) | rpl::on_next([=]() mutable {
				if (finalCheck()) {
					saveToFiles();
					base::take(lifetime)->destroy();
				}
			}, *lifetime);
		}
	};
	const auto saveDocuments = [=](const QString &folderPath) {
		for (const auto &[document, origin] : documents) {
			if (!folderPath.isEmpty()) {
				const auto name =
					base::FileNameFromUserString(document->filename());
				const auto target = SexCat::AdjustDocumentSaveTarget(
					folderPath + name,
					document,
					origin);
				SexCat::MarkDocumentSaveAs(document, origin, batchTag);
				document->save(origin, target);
			} else {
				DocumentSaveClickHandler::SaveAndTrack(origin, document);
			}
		}
	};

	menu->addAction(text, [=] {
		ChooseFolderAndSave(
			weak,
			[=](const QString &folderPath) {
				saveImages(folderPath);
				saveDocuments(folderPath);
				callback();
			},
			forceFolderDialog);
	}, icon);
}

// Q50: pick a subset of an album's videos and download just those.
void AddDownloadVideosSelector(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		const Documents &documents) {
	auto videos = Documents();
	for (const auto &entry : documents) {
		const auto &document = entry.first;
		if (document->isVideoFile() || document->isAnimation()) {
			videos.push_back(entry);
		}
	}
	if (videos.size() < 2) {
		return;
	}
	const auto weak = base::make_weak(window);
	const auto state = std::make_shared<std::vector<bool>>(
		videos.size(),
		false);
	const auto anchor = menu->addAction(
		base::make_unique_q<Ui::Menu::Action>(
			menu->menu(),
			menu->menu()->st(),
			Ui::Menu::CreateAction(
				menu->menu().get(),
				tr::ayu_SelectVideosToDownload(tr::now),
				nullptr),
			&st::menuIconDownload,
			&st::menuIconDownload));
	anchor->setMenu(new QMenu(menu->menu().get()));
	const auto sub = menu->ensureSubmenu(anchor, st::popupMenuWithIcons);
	const auto showCount = [=](int count) {
		Ui::Toast::Show(Ui::Toast::Config{
			.text = TextWithEntities{
				tr::lng_sexcat_videos_selected(tr::now, lt_count, count),
			},
		});
	};
	const auto savePicked = [=](const QString &folderPath) {
		const auto controller = weak.get();
		if (!controller) {
			return;
		}
		const auto session = &controller->session();
		auto batchTag = QString();
		for (const auto &[document, fullId] : videos) {
			if (const auto item = session->data().message(fullId)) {
				batchTag = SexCat::SourceTagForItem(item);
				if (!batchTag.isEmpty()) {
					break;
				}
			}
		}
		for (auto i = 0; i < int(videos.size()); i++) {
			if (!(*state)[i]) {
				continue;
			}
			const auto &[document, origin] = videos[i];
			if (!folderPath.isEmpty()) {
				const auto name =
					base::FileNameFromUserString(document->filename());
				const auto target = SexCat::AdjustDocumentSaveTarget(
					folderPath + name,
					document,
					origin);
				SexCat::MarkDocumentSaveAs(document, origin, batchTag);
				document->save(origin, target);
			} else {
				DocumentSaveClickHandler::SaveAndTrack(origin, document);
			}
		}
	};
	for (auto i = 0; i < int(videos.size()); i++) {
		const auto index = i;
		const auto label = u"%1 · %2"_q.arg(i + 1).arg(
			FormatSizeText(videos[i].first->size));
		sub->addAction(label, [=] {
			auto &on = (*state)[index];
			on = !on;
			auto count = 0;
			for (const auto value : *state) {
				count += (value ? 1 : 0);
			}
			showCount(count);
		});
	}
	sub->addAction(
		tr::ayu_DownloadSelectedVideos(tr::now),
		[=] {
			auto picked = 0;
			for (const auto value : *state) {
				picked += (value ? 1 : 0);
			}
			if (!picked) {
				Ui::Toast::Show(Ui::Toast::Config{
					.text = TextWithEntities{
						tr::lng_sexcat_nothing_selected(tr::now),
					},
				});
				return;
			}
			ChooseFolderAndSave(weak, savePicked, false);
		},
		&st::menuIconDownload);
}

} // namespace

void AddDownloadFilesAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		const std::vector<HistoryView::SelectedItem> &selectedItems,
		not_null<HistoryView::ListWidget*> list) {
	if (selectedItems.empty()) {
		return;
	}
	auto docs = Documents();
	auto photos = Photos();
	for (const auto &selectedItem : selectedItems) {
		const auto &id = selectedItem.msgId;
		const auto item = window->session().data().message(id);

		if (!Collected(item, docs, photos)) {
			return;
		}
	}
	std::sort(docs.begin(), docs.end(), [](const auto &a, const auto &b) {
		return a.second < b.second;
	});
	std::sort(photos.begin(), photos.end(), [](const auto &a, const auto &b) {
		return a.second < b.second;
	});
	if (docs.empty() && photos.empty()) {
		return;
	}
	const auto done = [weak = base::make_weak(list)] {
		if (const auto strong = weak.get()) {
			strong->cancelSelection();
		}
	};
	AddAction(menu, window, docs, photos, done);
	AddAction(
		menu,
		window,
		docs,
		photos,
		done,
		tr::ayu_DownloadFilesAs(tr::now),
		nullptr,
		true);
}

void AddDownloadFilesAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		const std::vector<not_null<HistoryItem*>> &items,
		not_null<HistoryInner*> list) {
	if (items.empty()) {
		return;
	}
	auto docs = Documents();
	auto photos = Photos();
	for (const auto &item : items) {
		if (!Collected(item, docs, photos)) {
			return;
		}
	}
	std::sort(docs.begin(), docs.end(), [](const auto &a, const auto &b) {
		return a.second < b.second;
	});
	std::sort(photos.begin(), photos.end(), [](const auto &a, const auto &b) {
		return a.second < b.second;
	});
	if (docs.empty() && photos.empty()) {
		return;
	}
	const auto done = [weak = base::make_weak(list)] {
		if (const auto strong = weak.get()) {
			strong->clearSelected();
		}
	};
	AddAction(menu, window, docs, photos, done);
	AddAction(
		menu,
		window,
		docs,
		photos,
		done,
		tr::ayu_DownloadFilesAs(tr::now),
		nullptr,
		true);
}

void AddDownloadAlbumAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		HistoryItem *item) {
	if (!item) {
		return;
	}
	const auto group = item->history()->owner().groups().find(item);
	if (!group || group->items.size() < 2) {
		return;
	}
	auto docs = Documents();
	auto photos = Photos();
	for (const auto &groupItem : group->items) {
		if (!Collected(groupItem.get(), docs, photos)) {
			return;
		}
	}
	if (docs.empty() && photos.empty()) {
		return;
	}
	AddAction(
		menu,
		window,
		docs,
		photos,
		[] {},
		tr::ayu_DownloadAlbumAll(tr::now),
		&st::menuIconDownload);
	AddAction(
		menu,
		window,
		docs,
		photos,
		[] {},
		tr::ayu_DownloadAlbumAs(tr::now),
		&st::menuIconDownload,
		true);
	AddDownloadVideosSelector(menu, window, docs);
}

} // namespace Menu
