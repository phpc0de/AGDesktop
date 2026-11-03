// Hand-written one-line docs for common TL constructors, shown inline by
// mtp_tl_codec.cpp. fields format: "字段名:说明;字段名:说明".
#pragma once

#include <cstring>

namespace Ayu::Debug {

struct CtorDoc {
	const char *name;
	const char *purpose;
	const char *fields;
};

inline constexpr CtorDoc kCtorDocs[] = {
	// requests
	{ "updates.getState",
		"取当前更新状态序号，用于计算 getDifference 的起点",
		"" },
	{ "updates.getDifference",
		"拉取 pts/qts 之后的全部更新增量",
		"pts:本地已应用的用户事件序号;pts_limit:每包最多事件数"
		";pts_total_limit:事件总数上限;date:本地时间戳;qts:本地已应用的秘密聊天事件序号"
		";qts_limit:每包最多秘密事件数" },
	{ "updates.getChannelDifference",
		"频道增量拉取，pts 起点越大返回越少",
		"force:即使无新事件也强制返回一包;channel:目标频道"
		";filter:要拉取的更新类型;pts:本地已应用的频道事件序号;limit:单包最多条数" },
	{ "messages.getHistory",
		"拉取历史消息，返回 offset_id 之前 limit 条",
		"peer:目标会话;offset_id:翻页起点消息 id（返回比它旧的消息）"
		";offset_date:按日期翻页;add_offset:在 offset 基础上额外跳过的条数"
		";limit:最多返回条数;max_id:只返回比它旧的消息;min_id:只返回比它新的消息"
		";hash:本地缓存校验值（相等返回空）" },
	{ "messages.getDialogs",
		"拉取会话列表（分页）",
		"exclude_pinned:排除置顶会话;folder_id:文件夹编号"
		";offset_date:分页锚点（日期）;offset_id:分页锚点（消息 id）"
		";offset_peer:分页锚点（会话）;limit:最多返回条数;hash:本地缓存校验值" },
	{ "messages.search",
		"按关键词搜索消息",
		"peer:搜索的会话;q:搜索关键词;from_id:限定发送者;saved_peer_id:限定收藏对话"
		";top_msg_id:限定话题;filter:消息类型过滤;min_date:时间范围起点"
		";max_date:时间范围终点;offset_id:翻页起点消息 id;add_offset:额外跳过条数"
		";limit:最多返回条数;max_id:只返回比它旧的消息;min_id:只返回比它新的消息"
		";hash:本地缓存校验值" },
	{ "messages.sendMessage",
		"发送文本消息",
		"no_webpage:不生成网页预览;silent:静音推送;background:后台发送"
		";clear_draft:同时清空草稿;noforwards:禁止转发;peer:目标会话"
		";reply_to:回复的消息;message:文本内容;random_id:客户端随机 id（服务器去重用）"
		";reply_markup:消息按钮;entities:文本格式实体;schedule_date:定时发送时间"
		";send_as:以某身份发送;effect:消息特效" },
	{ "messages.sendMedia",
		"发送单条媒体（图片、文件、投票等）",
		"peer:目标会话;reply_to:回复的消息;media:媒体描述（InputMedia）"
		";message:说明文字;random_id:客户端随机 id（服务器去重用）"
		";entities:说明文字实体;schedule_date:定时发送时间;send_as:以某身份发送" },
	{ "messages.sendMultiMedia",
		"相册一次发送多条媒体",
		"peer:目标会话;reply_to:回复的消息;multi_media:每条媒体的描述与说明"
		";schedule_date:定时发送时间;send_as:以某身份发送" },
	{ "messages.forwardMessages",
		"转发消息，id 与 random_id 一一对应",
		"silent:静音推送;from_peer:来源会话;id:要转发的消息 id 列表"
		";random_id:与 id 一一对应的随机 id;to_peer:目标会话;top_msg_id:转发进的话题"
		";drop_author:隐藏来源;drop_media_captions:去掉说明文字"
		";schedule_date:定时发送时间;send_as:以某身份发送" },
	{ "messages.deleteMessages",
		"删除消息，revoke 为真时同时从对方撤回",
		"revoke:同时从对方撤回;id:要删除的消息 id 列表" },
	{ "messages.readHistory",
		"把会话已读位置推进到 max_id",
		"peer:目标会话;max_id:已读到的消息 id" },
	{ "channels.getMessages",
		"按消息 id 拉取频道消息",
		"channel:目标频道;id:要拉取的消息 id 列表" },
	{ "channels.readHistory",
		"把频道已读位置推进到 max_id",
		"channel:目标频道;max_id:已读到的消息 id" },
	{ "upload.getFile",
		"按 offset/limit 分块下载文件，大文件 limit 不超过 1MB",
		"precise:按字节精确返回;cdn_supported:允许 CDN 分发;location:文件位置"
		";offset:起始字节偏移;limit:本次请求的字节数" },
	{ "upload.saveFilePart",
		"上传小文件的一个分片",
		"file_id:上传会话 id;file_part:分片序号;bytes:分片数据（不超过 128KB）" },
	{ "upload.saveBigFilePart",
		"上传大文件的一个分片",
		"file_id:上传会话 id;file_part:分片序号;file_total_parts:总分片数"
		";bytes:分片数据（不超过 512KB）" },
	{ "contacts.resolveUsername",
		"把用户名解析成 peer 与其 id",
		"username:目标用户名（不带 @）;referer:来源标记" },
	{ "help.getConfig",
		"拉取服务器配置（DC 列表、限速、功能开关等）",
		"" },
	// responses
	{ "updates.state",
		"返回当前更新状态序号",
		"pts:已应用的用户事件序号;qts:已应用的秘密事件序号;date:服务器时间"
		";seq:更新序列号;unread_count:未读消息数" },
	{ "updates.difference",
		"两个状态之间的全部更新增量",
		"new_messages:新消息列表;new_encrypted_messages:新加密消息"
		";other_updates:其他更新事件;chats:补充的群与频道资料;users:补充的用户资料"
		";state:应用本包后的新状态" },
	{ "updates.channelDifference",
		"频道增量包，final 为真表示已是最后一包",
		"final:true 表示已追平;pts:应用后的频道序号;timeout:建议长轮询秒数"
		";new_messages:新消息列表;other_updates:其他更新事件;chats:补充的频道资料"
		";users:补充的用户资料" },
	{ "updates.channelDifferenceTooLong",
		"频道落后太多，直接返回全量并切换状态",
		"final:true 表示已切换为全量;timeout:建议长轮询秒数"
		";dialog:该频道的全量会话信息;messages:最新消息列表;chats:频道资料"
		";users:用户资料" },
	{ "messages.messages",
		"消息列表响应",
		"messages:消息列表;topics:话题列表;chats:涉及的群与频道;users:涉及的用户" },
	{ "messages.dialogs",
		"会话列表响应",
		"dialogs:会话列表;messages:各会话最后一条消息;chats:涉及的群与频道"
		";users:涉及的用户" },
	{ "updates.updates",
		"常规更新推送信封",
		"updates:更新事件列表;users:补充的用户资料;chats:补充的群与频道资料"
		";date:服务器时间;seq:更新序列号" },
	// transport envelope
	{ "message",
		"单条传输消息信封",
		"msg_id:服务器消息 id（随时间递增）;seqno:会话内序列号;bytes:载荷字节数"
		";body:载荷 TL 对象" },
	{ "msg_container",
		"把多条传输消息打进一个包",
		"messages:打包的多条传输消息" },
	{ "rpc_result",
		"RPC 响应信封，req_msg_id 对应请求",
		"req_msg_id:对应请求的消息 id;result:响应载荷" },
	{ "rpc_error",
		"RPC 请求失败",
		"error_code:错误码;error_message:错误标识（如 FLOOD_WAIT_X）" },
	{ "gzip_packed",
		"gzip 压缩的响应载荷",
		"packed_data:解压后的 TL 数据" },
	{ "new_session_created",
		"服务器新建会话，需要换用新盐",
		"first_msg_id:此后消息可用的起始 id;unique_id:会话唯一 id（去重用）"
		";server_salt:新的盐值" },
	{ "bad_server_salt",
		"盐值过期，重发前先换新盐",
		"bad_msg_id:被拒的消息 id;bad_msg_seqno:被拒消息序列号;error_code:拒绝原因码"
		";new_server_salt:需改用的盐值" },
	{ "bad_msg_notification",
		"消息被拒收（序号或时间错误）",
		"bad_msg_id:被拒的消息 id;bad_msg_seqno:被拒消息序列号;error_code:拒绝原因码" },
	{ "msgs_ack",
		"确认已收到这些消息",
		"msg_ids:已确认的消息 id 列表" },
};

[[nodiscard]] inline const CtorDoc *FindCtorDoc(const char *name) {
	for (const auto &doc : kCtorDocs) {
		if (std::strcmp(doc.name, name) == 0) {
			return &doc;
		}
	}
	return nullptr;
}

} // namespace Ayu::Debug
