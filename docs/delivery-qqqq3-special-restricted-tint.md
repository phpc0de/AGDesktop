# 交付：QQQQ3 特殊限制频道染色（G1–G4）

## 症状

顶栏群名（Q3.1）与列表频道名（Q3.2）对"特殊限制频道"永远全黑，tint 特性四轮修复（18c8fd7957 / 9cb8ededbe / 5272ab2778 / 0d0c001b68）全部无效。

## 根因

用户回执定案：频道打开后"除了没有消息之外，其他一模一样"（无限制提示页）+ 官方默认主题。⇒ 这类频道是服务端**静默拒发消息**：peer 按普通频道下发，`Forbidden` / `is_restricted` / `noforwards` 一个旗标都不带，`Ayu::PeerTintOf` 恒 None。前四轮修的全是"服务器旗标可见性"，目标集合本身在数据里不存在。

客户端唯一可观测信号：**首载空历史**（`HistoryWidget::firstLoadMessages` 的 `messages.getHistory` 返回 0 条）或**首载报错**（`messagesFailed` 的 CHANNEL_PRIVATE 族，原行为仅关窗+toast，不落状态）。路线图原定挂在 apiwrap.cpp:3680/3690，排查确认首载真实路径是 history_widget.cpp:5029（`histories.sendRequest`），已改挂；apiwrap 的跳转/搜索路径未覆盖。

## 改动

- **G1** `history/history_widget.cpp` `firstLoadMessages` done：`FirstLoadCameBackEmpty`（对 messages / messagesSlice / channelMessages 判 `vmessages` 空，messagesNotModified 不算空）→ `MarkSilentlyRestrictedChannel`。
- **G2** 同文件 `messagesFailed` CHANNEL_PRIVATE / CHANNEL_PUBLIC_GROUP_NA / USER_BANNED_IN_CHANNEL 族 → 同 G1；close/toast 行为不动。
- **G3** `ayu/ayu_settings.h/.cpp`：`std::unordered_set<uint64> _specialRestrictedPeers` + `isSpecialRestrictedPeer` / `setSpecialRestrictedPeer`，镜像 shadowBanIds 的 json 直序列化（写读双向），随 settings 落盘，跨重启持久。
- **G4** `ayu/ui/peer_tint.cpp` 频道分支最前查集合 → `PeerTint::Banned`（红）。列表名与顶栏名共用同一判定，一处置位两处全亮。
- `MarkSilentlyRestrictedChannel`（history_widget.cpp 匿名 ns）：置位幂等 + `updateChatListEntry()` + `peerUpdated(PeerUpdateFlag::UnavailableReason)`，列表行与顶栏同拍重绘。

## 行为边界（如实告知）

染色自该频道**首次打开之后**生效并跨重启持久；从未打开过的静默受限频道没有任何客户端信号可辨，仍是黑。不新增 ChannelData 旗标（AyuForbidden 会被全量 apply 清掉，复用必死）。

## 外部构建机手测清单

1. 打开一个"特殊限制频道"（打开后无消息）→ 聊天窗口顶栏群名立即变红、底部出现 2px 色条。
2. 退回左侧列表 → 该频道名字变红。
3. 重启应用 → 列表里该频道名字仍是红（settings.json 出现 `"specialRestrictedPeers": ["<id>"]`）。
4. 打开任意普通频道 → 两处均不变色。
5. （可选）用一台能正常拉历史的小号开同一频道 → 该号不染色（信号本身是账号级的）。
