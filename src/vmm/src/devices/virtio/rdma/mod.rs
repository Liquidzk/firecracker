// Copyright 2025 Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

pub mod device;
mod event_handler;

pub use self::device::{RdmaError, VirtioRdma};

pub(crate) const RDMA_NUM_QUEUES: usize = 3;
pub(crate) const RDMA_CTRL_QUEUE: usize = 0;
pub(crate) const RDMA_DATA_QUEUE: usize = 1;
pub(crate) const RDMA_CQ_QUEUE: usize = 2;
