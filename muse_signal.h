/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "cJSON.h"

/* Signal buttons (CONFIG_HOMEHUB_SIGNAL_BUTTON): starts the sender
 * task and loads the button set through the chat (a CFGREQ signal
 * asks, the agent answers with a [CFG] message read back over
 * GET /chat/history). A press posts one "[SIG] code=<code> ..."
 * chat message; see muse_signal.c. */
void muse_signal_init(void);

/* Result object for the device.signal command: the last signal this
 * device sent (seq, code, whether the VM acknowledged it), and
 * whether one is still pending. Caller owns the returned object. */
cJSON *muse_signal_last_json(void);
