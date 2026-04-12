# P2P File Sharing System

ソケット通信を用いた P2P ファイル共有システムを C++ で実装。集中型レジストリと複数ピア間でのファイル検索・転送機能を、スレッドや外部ライブラリを使わずに poll() ベースのイベントループのみで実現した。

## 概要

- **registry**: 中央レジストリ。peer の登録、ファイル一覧、検索を管理
- **peer**: 各ノード。registry に登録し、他 peer とファイルを直接やりとり

JOIN / PUBLISH / SEARCH / FETCH の独自バイナリプロトコルを設計。すべての I/O は非ブロッキングで、単一プロセス・単一スレッドで複数接続を同時処理する。

## 実装上の特徴

- poll() による多重 I/O(select の FD_SETSIZE 制限を回避)
- バイナリプロトコル(ネットワークバイトオーダー、固定長 + 可変長フィールド)
- 自分自身を相手にした FETCH のデッドロック検出と回避
- リソースリークを避けるための明示的な socket クローズ管理
  
## ビルド

make

レジストリ起動:
./registry <registry_port>

ピア起動:
./peer <registry_host> <registry_port> <peer_id> <listen_port>

ピアのコマンド: `JOIN`, `PUBLISH`, `SEARCH`, `FETCH`, `EXIT`
## セキュリティ上の注意

すべてのプロトコルメッセージは TLS 等で保護されておらず、平文で送信される。`tcpdump` で loopback インターフェースをキャプチャすると、ファイル名やクエリが ASCII でそのまま観察できる。

<img width="1920" height="1026" alt="plaintext" src="https://github.com/user-attachments/assets/11f7edcc-3a4a-4d32-8fd4-7650293ac4b3" />
上のキャプチャでは、PUBLISH されたファイル名 (hello.txt や test.txt など) と FETCH クエリ（capture.txt) が平文で確認できる。

また、FETCH で受信したファイル名を `SharedFiles/` に直接連結しているため、パストラバースにも対応していない。
