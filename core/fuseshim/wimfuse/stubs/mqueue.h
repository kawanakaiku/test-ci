/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Android (bionic) には <mqueue.h> が無い。wimlib の mount_image.c は POSIX メッセージキューを、アンマウントの要求の送受信 (別プロセスの wimlib_unmount_image と、
 * 書き込みマウントの commit の進捗) にだけ使う。wimfuse は読み取り専用で、アンマウントは shim が fuse_exit で直接行う (wimlib_unmount_image は呼ばない) ので、
 * 宣言だけの代用ヘッダと、常に失敗する実装 (wimfuse_io.c) で足りる。wimlib のソースは無改変で、このヘッダをインクルードパスの先頭に置いてビルドする。 */
#ifndef WIMFUSE_STUB_MQUEUE_H
#define WIMFUSE_STUB_MQUEUE_H
#include <stddef.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int mqd_t;
struct mq_attr {
  long mq_flags, mq_maxmsg, mq_msgsize, mq_curmsgs, __pad[4];
};
mqd_t mq_open(const char *name, int oflag, ...);
int mq_close(mqd_t mq);
int mq_unlink(const char *name);
int mq_send(mqd_t mq, const char *msg, size_t len, unsigned prio);
ssize_t mq_receive(mqd_t mq, char *msg, size_t len, unsigned *prio);
#ifdef __cplusplus
}
#endif
#endif
