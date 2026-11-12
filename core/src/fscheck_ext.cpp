// SPDX-License-Identifier: Apache-2.0
#include "fscheck_internal.h"
namespace usbmem { namespace fsck {
Status check_ext(Ctx&, FsType) { return Status::Make(Errc::Unsupported); }
}}
