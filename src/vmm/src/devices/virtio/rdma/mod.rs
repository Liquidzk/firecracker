// Copyright 2025 Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

pub mod device;
mod event_handler;

pub use self::device::{RdmaError, VirtioRdma};

pub(crate) const RDMA_NUM_QUEUES: usize = 1;
pub(crate) const RDMA_QUEUE: usize = 0;
