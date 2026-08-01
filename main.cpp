// ============================================================
// TinyRPC 游戏服务端入口
//
// 启动端口 8080，监听客户端连接。
// 支持：房间（8个RPC） + 帧同步（SendInput/StartGame） + 匹配 + 断连清理
// ============================================================

#include "game/game_service.h"

#include <cstdio>

int main() {
    setbuf(stdout, NULL);  // Docker 容器里 stdout 默认全缓冲，显式禁用
    printf("=== TinyRPC 游戏服务端 ===\n\n");

    game::GameService server;

    // 阻塞运行，直到 Stop() 被调用
    server.Run(8080);

    return 0;
}
