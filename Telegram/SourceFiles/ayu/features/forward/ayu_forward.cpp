// This is the source code of [R:67494147] for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/forward/ayu_forward.h"

#include "apiwrap.h"
#include "api/api_sending.h"
#include "api/api_text_entities.h"
#include "lang_auto.h"
#include "ayu/features/forward/ayu_forward_rich.h"
#include "ayu/features/forward/ayu_sync.h"
#include "ayu/ayu_settings.h"
#include "ayu/utils/ayu_log.h"
#include "ayu/utils/telegram_helpers.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "storage/localimageloader.h"
#include "storage/storage_account.h"
#include "storage/storage_media_prepare.h"
#include "styles/style_boxes.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/text/text_utilities.h"

#include <map>
#include <QMutex>

namespace AyuForward {

std::unordered_map<PeerId, std::shared_ptr<ForwardState>> forwardStates;
QMutex forwardStatesMutex;

std::shared_ptr<ForwardState> FindForwardState(const PeerId &id) {
	QMutexLocker lock(&forwardStatesMutex);
	const auto i = forwardStates.find(id);
	return (i == forwardStates.end()) ? nullptr : i->second;
}

void SetForwardState(
		const PeerId &id,
		std::shared_ptr<ForwardState> state) {
	QMutexLocker lock(&forwardStatesMutex);
	forwardStates[id] = std::move(state);
}

bool isForwarding(const PeerId &id) {
	const auto fwState = FindForwardState(id);
	if (id.value && fwState) {
		const auto &state = *fwState;

		return state.state != ForwardState::State::Finished
			&& state.currentChunk < state.totalChunks
			&& !state.stopRequested
			&& ((state.totalChunks && state.totalMessages) || state.state == ForwardState::State::Downloading);
	}
	return false;
}

void cancelForward(const PeerId &id, const Main::Session &session) {
	if (const auto fwState = FindForwardState(id)) {
		fwState->stopRequested = true;
		fwState->updateBottomBar(session, &id, ForwardState::State::Finished);
	}
}

std::pair<QString, QString> stateName(const PeerId &id) {
	const auto state = FindForwardState(id);
	if (!state) {
		return std::make_pair(QString(), QString());
	}

	QString messagesString = tr::ayu_AyuForwardStatusSentCount(tr::now,
															   lt_count1,
															   QString::number(state->sentMessages),
															   lt_count2,
															   QString::number(state->totalMessages)

	);

	QString chunkString = tr::ayu_AyuForwardStatusChunkCount(tr::now,
															 lt_count1,
															 QString::number(state->currentChunk + 1),
															 lt_count2,
															 QString::number(state->totalChunks)

	);

	const auto partString = state->totalChunks <= 1 ? messagesString : (messagesString + " • " + chunkString);

	QString status;

	if (state->state == ForwardState::State::Preparing) {
		status = tr::ayu_AyuForwardStatusPreparing(tr::now);
	} else if (state->state == ForwardState::State::Downloading) {
		return std::make_pair(tr::ayu_AyuForwardStatusLoadingMedia(tr::now), "");
	} else if (state->state == ForwardState::State::Sending) {
		status = tr::ayu_AyuForwardStatusForwarding(tr::now);
	} else {
		// ForwardState::State::Finished
		status = tr::ayu_AyuForwardStatusFinished(tr::now);
	}


	return std::make_pair(status, partString);
}

void ForwardState::updateBottomBar(const Main::Session &session, const PeerId *peer, const State &st) {
	state = st;
	auto peerCopy = *peer;
	crl::on_main([&, peerCopy]
	{
		session.changes().peerUpdated(session.data().peer(peerCopy), Data::PeerUpdate::Flag::Rights);
	});
}

static Ui::PreparedList prepareMedia(not_null<Main::Session*> session,
									 const std::vector<not_null<HistoryItem*>> &items,
									 int &i,
									 std::vector<not_null<Data::Media*>> &groupMedia,
									 const AyuSync::DocumentPaths &documentPaths) {
	auto list = Ui::PreparedList();
	const auto append = [&](not_null<Data::Media*> media)
	{
		auto path = QString();
		const auto document = media->document();
		if (document) {
			const auto j = documentPaths.find(document);
			if (j != documentPaths.end()) {
				path = j->second;
			}
		} else if (const auto photo = media->photo()) {
			path = AyuSync::filePath(session, photo);
		}
		auto prepared = Ui::PreparedFile(path);
		if (prepared.path.isEmpty()) {
			return;
		}
		if (document) {
			prepared.displayName = AyuSync::documentFileName(document);
		}
		Storage::PrepareDetails(prepared, st::sendMediaPreviewSize, PhotoSideLimit());
		groupMedia.emplace_back(media);
		list.files.emplace_back(std::move(prepared));
	};

	const auto startItem = items[i];
	const auto media = startItem->media();
	const auto groupId = startItem->groupId();

	append(media);

	if (!groupId.value) {
		return list;
	}

	for (int k = i + 1; k < items.size(); ++k) {
		const auto nextItem = items[k];
		if (nextItem->groupId() != groupId) {
			break;
		}
		if (const auto nextMedia = nextItem->media()) {
			append(nextMedia);
			i = k;
		}
	}
	return list;
}

void sendMedia(
	not_null<Main::Session*> session,
	const std::shared_ptr<Ui::PreparedBundle> &bundle,
	not_null<Data::Media*> primaryMedia,
	Api::MessageToSend &&message,
	bool sendImagesAsPhotos) {
	if (const auto document = primaryMedia->document(); document && document->sticker()) {
		AyuSync::sendStickerSync(session, std::move(message), document);
		return;
	}

	auto mediaType = [&]
	{
		if (const auto document = primaryMedia->document()) {
			if (document->isVoiceMessage()) {
				return SendMediaType::Audio;
			} else if (document->isVideoMessage()) {
				return SendMediaType::Round;
			} else if (document->isVideoFile() || document->isGifv()) {
				// to send video as video need to pass it as 'photo'
				// ref: `void HistoryWidget::sendingFilesConfirmed`
				return SendMediaType::Photo;
			}
			return SendMediaType::File;
		}
		return SendMediaType::Photo;
	}();

	if (mediaType == SendMediaType::Round || mediaType == SendMediaType::Audio) {
		const auto path = bundle->groups.front().list.files.front().path;

		QFile file(path);
		auto failed = false;
		if (!file.open(QIODevice::ReadOnly)) {
			LOG(("failed to open file for forward with reason: %1").arg(file.errorString()));
			failed = true;
		}
		auto data = file.readAll();

		if (!failed && data.size()) {
			file.close();
			AyuSync::sendVoiceSync(session,
								   data,
								   primaryMedia->document()->duration(),
								   mediaType == SendMediaType::Round,
								   std::move(message));
			return;
		}
		// at least try to send it as squared-video
	}

	// workaround for media albums consisting of video and photos
	if (sendImagesAsPhotos) {
		mediaType = SendMediaType::Photo;
	}

	for (auto &group : bundle->groups) {
		AyuSync::sendDocumentSync(
			session,
			group,
			mediaType,
			std::move(message.textWithTags),
			message.action);
	}
}

bool isAyuForwardNeeded(const std::vector<not_null<HistoryItem*>> &items) {
	const auto needAyuForward = [&](const auto &item)
	{
		return isAyuForwardNeeded(item);
	};
	return std::ranges::any_of(items, needAyuForward);
}

bool isAyuForwardNeeded(not_null<HistoryItem*> item) {
	if (item->isDeleted() || item->isAyuNoForwards() || (item->media() && item->media()->ttlSeconds())) {
		return true;
	}
	return false;
}

bool isFullAyuForwardNeeded(not_null<HistoryItem*> item) {
	return item->from()->isAyuNoForwards() || item->history()->peer->isAyuNoForwards();
}

struct ForwardChunk
{
	bool isAyuForwardNeeded;
	std::vector<not_null<HistoryItem*>> items;
};

void intelligentForward(
	not_null<Main::Session*> session,
	const Api::SendAction &action,
	const Data::ResolvedForwardDraft &draft) {
	const auto history = action.history;
	const auto topicRootId = action.replyTo.topicRootId;
	const auto monoforumPeerId = action.replyTo.monoforumPeerId;
	crl::on_main([=]
	{
		history->setForwardDraft(topicRootId, monoforumPeerId, {});
	});

	const auto items = draft.items;
	if (items.empty()) {
		return;
	}
	const auto peer = history->peer;

	auto chunks = std::vector<ForwardChunk>();
	auto currentArray = std::vector<not_null<HistoryItem*>>();

	auto currentChunk = ForwardChunk({
		.isAyuForwardNeeded = isAyuForwardNeeded(items[0]),
		.items = currentArray
	});

	for (const auto &item : items) {
		if (isAyuForwardNeeded(item) != currentChunk.isAyuForwardNeeded) {
			currentChunk.items = currentArray;
			chunks.push_back(currentChunk);

			currentArray = std::vector<not_null<HistoryItem*>>();

			currentChunk = ForwardChunk({
				.isAyuForwardNeeded = isAyuForwardNeeded(item),
				.items = currentArray
			});
		}
		currentArray.push_back(item);
	}

	currentChunk.items = currentArray;
	chunks.push_back(currentChunk);

	auto state = std::make_shared<ForwardState>(chunks.size());
	SetForwardState(peer->id, state);


	for (const auto &chunk : chunks) {
		if (chunk.isAyuForwardNeeded) {
			forwardMessages(session, action, true, Data::ResolvedForwardDraft(chunk.items));
		} else {
			state->totalMessages = chunk.items.size();
			state->sentMessages = 0;
			state->updateBottomBar(*session, &peer->id, ForwardState::State::Sending);

			AyuSync::forwardMessagesSync(session, chunk.items, action, draft.options);

			state->sentMessages = state->totalMessages;

			state->updateBottomBar(*session, &peer->id, ForwardState::State::Finished);
		}
		state->currentChunk++;
	}

	state->updateBottomBar(*session, &peer->id, ForwardState::State::Finished);
}

[[nodiscard]] bool reattachExistingMedia(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		const std::vector<not_null<HistoryItem*>> &items,
		const std::shared_ptr<ForwardState> &state,
		const Api::SendAction &action) {
	for (const auto item : items) {
		const auto media = item->media();
		if (!media
			|| media->poll()
			|| (!media->document() && !media->photo())) {
			Ayu::Log::Write(
				u"forward"_q,
				u"reattach unsupported item=%1"_q.arg(item->id.bare));
			return false;
		}
	}
	state->updateBottomBar(*session, &peer->id, ForwardState::State::Sending);

	auto ids = std::vector<FullMsgId>();
	ids.reserve(items.size());
	auto documentOrigins = std::map<DocumentData*, FullMsgId>();
	auto photoOrigins = std::map<PhotoData*, FullMsgId>();
	for (const auto item : items) {
		ids.push_back(item->fullId());
		const auto media = item->media();
		if (const auto document = media->document()) {
			documentOrigins.emplace(document, item->fullId());
		} else if (const auto photo = media->photo()) {
			photoOrigins.emplace(photo, item->fullId());
		}
	}

	const auto sendItems = [=] {
		for (const auto id : ids) {
			const auto item = session->data().message(id);
			const auto media = item ? item->media() : nullptr;
			if (!media
				|| media->poll()
				|| (!media->document() && !media->photo())) {
				Ayu::Log::Write(
					u"forward"_q,
					u"reattach skip lost item=%1"_q.arg(id.msg.bare));
				continue;
			}
			auto message = Api::MessageToSend(action);
			message.action.options.invertCaption = item->invertMedia();
			message.action.options.scheduled = 0;
			message.action.options.suggest = {};
			message.action.options.effectId = 0;
			message.action.replaceMediaOf = 0;
			message.textWithTags = extractText(item);
			if (const auto document = media->document()) {
				Ayu::Log::Write(
					u"forward"_q,
					u"reattach document item=%1 dest=%2"_q
						.arg(item->id.bare)
						.arg(peer->id.value));
				Api::SendExistingDocument(std::move(message), document);
			} else {
				Ayu::Log::Write(
					u"forward"_q,
					u"reattach photo item=%1 dest=%2"_q
						.arg(item->id.bare)
						.arg(peer->id.value));
				Api::SendExistingPhoto(std::move(message), media->photo());
			}
		}
		state->updateBottomBar(
			*session,
			&peer->id,
			ForwardState::State::Finished);
		Ayu::Log::Write(
			u"forward"_q,
			u"reattach done items=%1 dest=%2"_q
				.arg(ids.size())
				.arg(peer->id.value));
	};

	// WHY: file_reference of source media expires after hours and
	// SendExistingDocument/SendExistingPhoto carry no origin for plain
	// media, so refresh every source message before the send loop.
	const auto remaining = std::make_shared<int>(
		int(documentOrigins.size() + photoOrigins.size()));
	const auto proceeded = std::make_shared<bool>(false);
	const auto proceed = [=] {
		if (*proceeded || *remaining > 0) {
			return;
		}
		*proceeded = true;
		Ayu::Log::Write(
			u"forward"_q,
			u"reattach refreshed continue items=%1 dest=%2"_q
				.arg(ids.size())
				.arg(peer->id.value));
		sendItems();
	};
	for (const auto &entry : documentOrigins) {
		const auto document = entry.first;
		const auto origin = entry.second;
		const auto usedReference = document->fileReference();
		session->api().refreshFileReference(
			Data::FileOrigin(origin),
			[=](const auto &) {
				const auto ok = document->fileReference() != usedReference;
				Ayu::Log::Write(
					u"forward"_q,
					u"reattach refresh %1 item=%2 document=%3"_q
						.arg(ok ? u"ok"_q : u"fail"_q)
						.arg(origin.msg.bare)
						.arg(document->id));
				(*remaining)--;
				proceed();
			});
	}
	for (const auto &entry : photoOrigins) {
		const auto photo = entry.first;
		const auto origin = entry.second;
		const auto usedReference = photo->fileReference();
		session->api().refreshFileReference(
			Data::FileOrigin(origin),
			[=](const auto &) {
				const auto ok = photo->fileReference() != usedReference;
				Ayu::Log::Write(
					u"forward"_q,
					u"reattach refresh %1 item=%2 photo=%3"_q
						.arg(ok ? u"ok"_q : u"fail"_q)
						.arg(origin.msg.bare)
						.arg(photo->id));
				(*remaining)--;
				proceed();
			});
	}
	proceed();
	return true;
}

void forwardMessages(
	not_null<Main::Session*> session,
	const Api::SendAction &action,
	bool forwardState,
	const Data::ResolvedForwardDraft &draft) {
	const auto items = draft.items;
	const auto history = action.history;
	const auto peer = history->peer;

	const auto topicRootId = action.replyTo.topicRootId;
	const auto monoforumPeerId = action.replyTo.monoforumPeerId;
	crl::on_main([=]
	{
		history->setForwardDraft(topicRootId, monoforumPeerId, {});
	});

	std::shared_ptr<ForwardState> state;

	if (forwardState) {
		const auto previous = FindForwardState(peer->id);
		state = previous
			? std::make_shared<ForwardState>(*previous)
			: std::make_shared<ForwardState>(1);
	} else {
		state = std::make_shared<ForwardState>(1);
	}

	SetForwardState(peer->id, state);

	Ayu::Log::Write(
		u"forward"_q,
		u"ayu start src=%1 author=%2 dest=%3 items=%4"_q
			.arg(items.front()->history()->peer->id.value)
			.arg(items.front()->from()->id.value)
			.arg(peer->id.value)
			.arg(items.size()));

	std::unordered_map<uint64, uint64> groupIds;

	std::vector<not_null<HistoryItem*>> toBeDownloaded;

	for (const auto item : items) {
		if (mediaDownloadable(item->media())) {
			toBeDownloaded.push_back(item);
		}

		if (item->groupId()) {
			const auto currentId = groupIds.find(item->groupId().value);

			if (currentId == groupIds.end()) {
				groupIds[item->groupId().value] = base::RandomValue<uint64>();
			}
		}
	}
	state->totalMessages = items.size();
	if (!toBeDownloaded.empty()
		&& reattachExistingMedia(session, peer, items, state, action)) {
		return;
	}

	auto documentPaths = AyuSync::DocumentPaths();
	if (!toBeDownloaded.empty()
		&& !AyuSettings::getInstance().experimentalForward()) {
		Ayu::Log::Write(
			u"forward"_q,
			u"experimental forward off: skip download-retransmit items=%1"
			" dest=%2"_q.arg(items.size()).arg(peer->id.value));
		state->updateBottomBar(
			*session,
			&peer->id,
			ForwardState::State::Finished);
		return;
	}
	if (!toBeDownloaded.empty()) {
		state->state = ForwardState::State::Downloading;
		state->updateBottomBar(*session, &peer->id, ForwardState::State::Downloading);
		documentPaths = AyuSync::loadDocuments(
			session,
			toBeDownloaded,
			[state] { return state->stopRequested.load(); });
	}
	if (state->stopRequested) {
		Ayu::Log::Write(u"forward"_q, u"ayu stopped before send dest=%1"_q.arg(peer->id.value));
		state->updateBottomBar(*session, &peer->id, ForwardState::State::Finished);
		return;
	}

	state->sentMessages = 0;
	state->updateBottomBar(*session, &peer->id, ForwardState::State::Sending);

	for (int i = 0; i < items.size(); i++) {
		const auto item = items[i];

		if (state->stopRequested) {
			state->updateBottomBar(*session, &peer->id, ForwardState::State::Finished);
			return;
		}
		const auto updateProgress = gsl::finally([&] {
			if (state->stopRequested) {
				return;
			}
			state->sentMessages = i + 1;
			state->updateBottomBar(
				*session,
				&peer->id,
				ForwardState::State::Sending);
		});

		if (item->richPage()) {
			state->updateBottomBar(*session, &peer->id, ForwardState::State::Downloading);

			const auto sent = forwardRichMessage(session, item->fullId(), action, [state]
			{
				return state->stopRequested.load();
			});

			if (state->stopRequested) {
				state->updateBottomBar(*session, &peer->id, ForwardState::State::Finished);
				return;
			}

			state->updateBottomBar(*session, &peer->id, ForwardState::State::Sending);

			if (sent) {
				continue;
			}
		}

		auto extractedText = extractText(item);
		if (extractedText.empty() && !mediaDownloadable(item->media())) {
			continue;
		}

		auto message = Api::MessageToSend(action);
		message.action.options.invertCaption = item->invertMedia();
		message.action.options.scheduled = 0;
		message.action.options.suggest = {};
		message.action.options.effectId = 0;
		message.action.replaceMediaOf = 0;

		if (draft.options != Data::ForwardOptions::NoNamesAndCaptions) {
			message.textWithTags = extractedText;
		}

		if (!mediaDownloadable(item->media())) {
			AyuSync::sendMessageSync(session, std::move(message));
		} else if (const auto media = item->media()) {
			if (media->poll()) {
				AyuSync::sendMessageSync(session, std::move(message));
				continue;
			}

			std::vector<not_null<Data::Media*>> groupMedia;
			auto preparedMedia = prepareMedia(
				session,
				items,
				i,
				groupMedia,
				documentPaths);

			// remove not finished files
			for (auto j = int(preparedMedia.files.size()); j != 0;) {
				--j;
				const auto &file = preparedMedia.files[j];

				const auto fileInfo = QFileInfo(file.path);
				const auto photo = groupMedia[j]->photo();
				const auto document = groupMedia[j]->document();
				const auto incompletePhoto = photo
					&& (!fileInfo.isFile()
						|| fileInfo.size()
							< photo->imageByteSize(Data::PhotoSize::Large));
				const auto incompleteDocument = document
					&& (!fileInfo.isFile()
						|| fileInfo.size() != document->size);
				if (incompletePhoto || incompleteDocument) {
					preparedMedia.files.erase(preparedMedia.files.begin() + j);
					groupMedia.erase(groupMedia.begin() + j);
				}
			}

			if (preparedMedia.files.empty()) {
				if (!message.textWithTags.empty()) {
					AyuSync::sendMessageSync(session, std::move(message));
				}
				continue;
			}

			auto way = Ui::SendFilesWay();
			way.setGroupFiles(true);
			way.setSendImagesAsPhotos(false);
			for (const auto &groupItem : groupMedia) {
				if (groupItem->photo()) {
					way.setSendImagesAsPhotos(true);
					break;
				}
			}

			auto groups = Ui::DivideByGroups(
				std::move(preparedMedia),
				way,
				peer->slowmodeApplied());

			auto bundle = Ui::PrepareFilesBundle(
				std::move(groups),
				way,
				false);
			sendMedia(
				session,
				bundle,
				groupMedia.front(),
				std::move(message),
				way.sendImagesAsPhotos());
		}
		// if there are grouped messages
		// "i" is incremented in prepareMedia
	}
	Ayu::Log::Write(u"forward"_q, u"ayu finished dest=%1 sent=%2"_q.arg(peer->id.value).arg(state->sentMessages));
	state->updateBottomBar(*session, &peer->id, ForwardState::State::Finished);
}

void zeroCostForward(
		not_null<Main::Session*> session,
		const Api::SendAction &action,
		const Data::ResolvedForwardDraft &draft) {
	const auto items = draft.items;
	const auto history = action.history;
	const auto peer = history->peer;
	Ayu::Log::Write(
		u"forward"_q,
		u"zerocost start dest=%1 items=%2"_q
			.arg(peer->id.value)
			.arg(items.size()));

	auto state = std::make_shared<ForwardState>(1);
	SetForwardState(peer->id, state);
	state->state = ForwardState::State::Sending;
	state->totalMessages = int(items.size());
	state->updateBottomBar(*session, &peer->id, ForwardState::State::Sending);

	// Group consecutive album items so media groups stay albums on wire.
	struct Unit {
		std::vector<not_null<HistoryItem*>> items;
	};
	std::vector<Unit> units;
	for (const auto item : items) {
		const auto media = item->media();
		const auto gid = item->groupId();
		const auto attachable = media
			&& !media->poll()
			&& !media->ttlSeconds()
			&& (media->photo() || media->document())
			&& !gid.empty();
		if (!attachable) {
			units.push_back({{item}});
			continue;
		}
		HistoryItem *prev = units.empty()
			? nullptr
			: units.back().items.front().get();
		const auto prevMedia = prev ? prev->media() : nullptr;
		if (prevMedia
			&& !prevMedia->poll()
			&& !prevMedia->ttlSeconds()
			&& (prevMedia->photo() || prevMedia->document())
			&& prev->groupId() == gid) {
			units.back().items.push_back(item);
			continue;
		}
		units.push_back({{item}});
	}

	auto remaining = std::make_shared<int>(0);
	auto fallback = std::make_shared<std::vector<not_null<HistoryItem*>>>();
	auto finished = std::make_shared<bool>(false);
	const auto settle = [=] {
		Ayu::Log::Write(
			u"forward"_q,
			u"zerocost settle sent=%1 total=%2 fallback=%3 dest=%4"_q
				.arg(state->sentMessages)
				.arg(state->totalMessages)
				.arg(fallback->size())
				.arg(peer->id.value));
		if (*finished || *remaining > 0) {
			return;
		}
		*finished = true;
		Ayu::Log::Write(
			u"forward"_q,
			u"zerocost done sent=%1 fallback=%2 dest=%3"_q
				.arg(state->sentMessages)
				.arg(fallback->size())
				.arg(peer->id.value));
		state->state = ForwardState::State::Finished;
		state->updateBottomBar(*session, &peer->id, ForwardState::State::Finished);
		if (!fallback->empty()) {
			Ayu::Log::Write(
				u"forward"_q,
				u"zerocost fallback full items=%1"_q.arg(fallback->size()));
			auto fallbackItems = std::move(*fallback);
			Ayu::Log::Write(
				u"forward"_q,
				u"zerocost fallback async dispatch items=%1 dest=%2"_q
					.arg(fallbackItems.size())
					.arg(peer->id.value));
			crl::async(crl::guard(session, [
				session,
				action,
				options = draft.options,
				items = std::move(fallbackItems)
			]() mutable {
				forwardMessages(
					session,
					action,
					false,
					Data::ResolvedForwardDraft{
						.items = std::move(items),
						.options = options,
					});
			}));
		}
	};
	const auto mediaToMtp = [](Data::Media *media) {
		if (media->photo()) {
			return MTPInputMedia(MTP_inputMediaPhoto(
				MTP_flags(0),
				media->photo()->mtpInput(),
				MTPint(), // ttl_seconds
				MTPInputDocument())); // video
		}
		return MTPInputMedia(MTP_inputMediaDocument(
			MTP_flags(0),
			media->document()->mtpInput(),
			MTPInputPhoto(), // video_cover
			MTPint(), // video_timestamp
			MTPint(), // ttl_seconds
			MTPstring())); // query
	};
	const auto captionOf = [options = draft.options](HistoryItem *item) {
		if (options == Data::ForwardOptions::NoNamesAndCaptions) {
			return TextWithEntities();
		}
		return item->originalText();
	};

	// WHY: per-unit sends below re-reference the original media and get
	// hard-rejected with CHAT_FORWARDS_RESTRICTED, so first probe a
	// drop-author copy forward, validated differently server-side.
	const auto probeUnsupported = [](HistoryItem *item) {
		const auto media = item->media();
		return item->isService()
			|| item->isDeleted()
			|| item->id.bare <= 0
			|| (media && (media->poll() || media->ttlSeconds()));
	};
	PeerData *probeFromPeer = nullptr;
	auto probeIds = QVector<MTPint>();
	auto probeRandomIds = QVector<MTPlong>();
	auto probePossible = !units.empty() && int(items.size()) <= 100;
	for (const auto item : items) {
		if (!probePossible
			|| probeUnsupported(item)
			|| (probeFromPeer && probeFromPeer != item->history()->peer.get())) {
			probePossible = false;
			break;
		}
		probeFromPeer = item->history()->peer.get();
		probeIds.push_back(MTP_int(item->id));
		probeRandomIds.push_back(MTP_long(base::RandomValue<uint64>()));
	}
	auto sendUnits = [=] {
		for (const auto &unit : units) {
			const auto item = unit.items.front();
			const auto media = item->media();
			if (!media || media->poll() || media->ttlSeconds()) {
				if (media && (media->poll() || media->ttlSeconds())) {
					fallback->push_back(item);
					continue;
				}
				auto extracted = extractText(item);
				if (extracted.empty()) {
					continue;
				}
				auto message = Api::MessageToSend(action);
				message.action.options.invertCaption = item->invertMedia();
				message.action.options.scheduled = 0;
				message.action.options.suggest = {};
				message.action.options.effectId = 0;
				message.action.replaceMediaOf = 0;
				if (draft.options != Data::ForwardOptions::NoNamesAndCaptions) {
					message.textWithTags = extracted;
				}
				session->api().sendMessage(std::move(message));
				state->sentMessages++;
				continue;
			}

			const auto mediaMtp = mediaToMtp(media);
			const auto caption = captionOf(item);
			const auto entities = Api::EntitiesToMTP(
				session,
				caption.entities,
				Api::ConvertOption::SkipLocal);
			if (unit.items.size() == 1) {
				(*remaining)++;
				session->api().request(MTPmessages_SendMedia(
					MTP_flags(MTPmessages_SendMedia::Flags(0)),
					peer->input(),
					MTPInputReplyTo(),
					mediaMtp,
					MTP_string(caption.text),
					MTP_long(base::RandomValue<uint64>()),
					MTPReplyMarkup(),
					entities,
					MTP_int(0), // schedule_date
					MTP_int(0), // schedule_repeat_period
					MTP_inputPeerEmpty(), // send_as
					MTPInputQuickReplyShortcut(),
					MTP_long(0), // effect
					MTP_long(0), // allow_paid_stars
					MTPSuggestedPost()
				)).done([=](const MTPUpdates &result) {
					Ayu::Log::Write(
						u"forward"_q,
						u"zerocost media ok item=%1 dest=%2"_q
							.arg(item->id.bare)
							.arg(peer->id.value));
					crl::on_main([=] {
						session->api().applyUpdates(result);
					});
					state->sentMessages++;
					(*remaining)--;
					settle();
				}).fail([=](const MTP::Error &error) {
					Ayu::Log::Write(
						u"forward"_q,
						u"zerocost media FAILED item=%1 dest=%2 type=%3"_q
							.arg(item->id.bare)
							.arg(peer->id.value)
							.arg(error.type()));
					fallback->push_back(item);
					(*remaining)--;
					settle();
				}).send();
				continue;
			}

			auto singles = QVector<MTPInputSingleMedia>();
			for (const auto groupItem : unit.items) {
				const auto groupCaption = captionOf(groupItem);
				singles.push_back(MTP_inputSingleMedia(
					MTP_flags(0),
					(groupItem == item)
						? mediaMtp
						: mediaToMtp(groupItem->media()),
					MTP_long(base::RandomValue<uint64>()),
					MTP_string(groupCaption.text),
					Api::EntitiesToMTP(
						session,
						groupCaption.entities,
						Api::ConvertOption::SkipLocal)));
			}
			(*remaining)++;
			session->api().request(MTPmessages_SendMultiMedia(
				MTP_flags(MTPmessages_SendMultiMedia::Flags(0)),
				peer->input(),
				MTPInputReplyTo(),
				MTP_vector<MTPInputSingleMedia>(std::move(singles)),
				MTP_int(0), // schedule_date
				MTP_inputPeerEmpty(), // send_as
				MTPInputQuickReplyShortcut(),
				MTP_long(0), // effect
				MTP_long(0) // allow_paid_stars
			)).done([=](const MTPUpdates &result) {
				Ayu::Log::Write(
					u"forward"_q,
					u"zerocost album ok count=%1 dest=%2"_q
						.arg(unit.items.size())
						.arg(peer->id.value));
				crl::on_main([=] {
					session->api().applyUpdates(result);
				});
				state->sentMessages += int(unit.items.size());
				(*remaining)--;
				settle();
			}).fail([=](const MTP::Error &error) {
				Ayu::Log::Write(
					u"forward"_q,
					u"zerocost album FAILED count=%1 dest=%2 type=%3"_q
						.arg(unit.items.size())
						.arg(peer->id.value)
						.arg(error.type()));
				for (const auto groupItem : unit.items) {
					fallback->push_back(groupItem);
				}
				(*remaining)--;
				settle();
			}).send();
		}
		settle();
	};

	if (!probePossible) {
		sendUnits();
		return;
	}

	using ProbeFlag = MTPmessages_ForwardMessages::Flag;
	const auto probeFlags = ProbeFlag() | ProbeFlag::f_drop_author
		| ProbeFlag::f_drop_media_captions;
	session->api().request(MTPmessages_ForwardMessages(
		MTP_flags(probeFlags),
		probeFromPeer->input(),
		MTP_vector<MTPint>(probeIds),
		MTP_vector<MTPlong>(probeRandomIds),
		peer->input(), // to_peer
		MTP_int(0), // top_msg_id
		MTPInputReplyTo(),
		MTP_int(0), // schedule_date
		MTP_int(0), // schedule_repeat_period
		MTP_inputPeerEmpty(), // send_as
		MTPInputQuickReplyShortcut(),
		MTP_long(0), // effect
		MTPint(), // video_timestamp
		MTP_long(0), // allow_paid_stars
		MTPSuggestedPost()
	)).done([=](const MTPUpdates &result) {
		Ayu::Log::Write(
			u"forward"_q,
			u"zerocost probe dropauthor ok items=%1"_q.arg(probeIds.size()));
		crl::on_main([=] {
			session->api().applyUpdates(result);
		});
		state->sentMessages = state->totalMessages;
		*remaining = 0;
		*finished = true;
		state->state = ForwardState::State::Finished;
		state->updateBottomBar(*session, &peer->id, ForwardState::State::Finished);
	}).fail([=, sendUnits = std::move(sendUnits)](const MTP::Error &error) {
		Ayu::Log::Write(
			u"forward"_q,
			u"zerocost probe dropauthor FAILED type=%1"_q.arg(error.type()));
		sendUnits();
	}).send();
}

} // namespace AyuForward
