# BonDriver_LinuxMirakc
Linux の EDCB で mirakc / Mirakurun に接続するための BonDriver です。

このリポジトリは [matching/BonDriver_LinuxMirakc](https://github.com/matching/BonDriver_LinuxMirakc) のフォークですが、フォーク元とは異なり **Mirakurunでのみ動作確認**しており、mirakcでは接続テストしていないので動くかわかりません。


以下をベースにLinuxで動くように移植しました。作者さまに感謝いたします。

・[BonDriver_mirakc](https://github.com/tkmsst/BonDriver_mirakc)

・[BonDriver_LinuxPTX](https://github.com/nns779/BonDriver_LinuxPTX)

オリジナルへの感謝はそのままに、いくつかのバグ修正・堅牢化を行っています。詳細は下記「フォークでの変更点」を参照してください。


## 設定ファイルについて

基本的には[BonDriver_mirakc](https://github.com/tkmsst/BonDriver_mirakc)のままですが、Unixドメインソケットに対応しています。
同サーバ内であれば利用できます。

以下のように設定ください。
```
SERVER_TYPE="unix"
SERVER_SOCKPATH="/var/run/mirakc.sock"
```

## ビルド方法

JSONの解析のために picojson が必要です。
include/picojson 配下に picojson のgitのツリーそのまま入れてください。

clone で行う方はする方は
> $ git clone "URL" ***--recurse-submodules***

とすることで picojson 含めてcloneされます。

ビルドはmakeコマンドにて実施してください。コンパイラは g++ です。
> $ make

## フォークでの変更点

オリジナル版に対して、主に以下の修正を行っています。実際の通信や長時間運用で発生しうる不具合の修正が中心で、機能や設定方法に変更はありません。

### バッファオーバーフロー対策
- mirakcからのHTTPレスポンス（ヘッダー・ボディ）を固定長バッファへコピーする箇所に上限チェックを追加し、想定外に大きなレスポンスでのオーバーフローを防止
- チャンネル/サービス一覧取得時のURL生成を `sprintf` から `snprintf` に変更
- 自身の `.so` ファイル名からチューナー名・iniファイルパスを生成する処理の境界チェック・整数アンダーフロー対策を追加

### スレッド安全性・同期まわり
- リングバッファ用ミューテックスの初期化不備（`pthread_mutexattr_init` の呼び忘れ）を修正
- `pthread_t` の値を `>` や真偽値で比較していた箇所を、専用フラグによる管理に変更（ポータビリティ対策）
- チャンネル切替時などに使う待機用条件変数を `CLOCK_REALTIME` から `CLOCK_MONOTONIC` に変更し、システム時刻の変更やNTP補正による待機時間の異常を防止
- `EnumChannelName` 等が返す `static` バッファを `thread_local` に変更し、複数スレッドからの同時呼び出しによる内容競合を防止
- ビットレート計算用の状態を関数内 `static` からインスタンスメンバに変更し、排他制御を統一
- ソケットの `shutdown()` を書き込み側のみ(`SHUT_WR`)から双方向(`SHUT_RDWR`)に変更し、受信スレッドがブロッキング`recv()`で永久に停止する問題（デッドロック）を修正
- チューナー終了時に、バッファ満杯で待機中の送信スレッドを確実に起こして終了させるシャットダウン機構を追加（`pthread_join`が返らなくなる問題への対応）
- チャンネル切替やチューナー終了時の状態不整合（古いチャンネル情報が残る、パージのタイミングでインデックスが壊れる等）を修正

### 例外・エラー処理
- JSON（picojson）へのアクセスや `pthread_create` の失敗判定など、エラー処理が機能していなかった、または例外が捕捉されずホストアプリごとクラッシュしうる箇所に対処
- プラグインのエントリポイント（`extern "C" CreateBonDriver`）を例外から保護

### リソース管理
- チャンネル切替失敗時やチューナー再オープン時のメモリリーク（`g_pType` 等）を修正
- 受信スレッドのバッファ確保をRAII化し、将来の変更でもリークしないように変更
- `GrabTsData` のミューテックス・条件変数の破棄漏れを修正

### 設定ファイル(ini)パーサ
- CRLF形式の設定ファイルで行末の `\r` が正しく除去されない不具合を修正
- セクション名解析でのバッファ範囲外アクセスの可能性を修正

### その他
- `IsTunerOpening()` が常に `FALSE` を返していたため、実際の接続状態を反映するよう修正
- `SetChannel` の空間境界チェックの不足、HTTPの1xx（暫定応答）受信時の処理不正などを修正

## License
This software is released under the MIT License, see LICENSE.
