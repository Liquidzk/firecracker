// Copyright 2025 Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

use std::io;
use std::ops::Deref;
use std::sync::Arc;

use vmm_sys_util::eventfd::EventFd;

use super::RDMA_NUM_QUEUES;
use crate::devices::virtio::ActivateError;
use crate::devices::virtio::device::{ActiveState, DeviceState, VirtioDevice, VirtioDeviceType};
use crate::devices::virtio::queue::{FIRECRACKER_MAX_QUEUE_SIZE, Queue};
use crate::devices::virtio::transport::VirtioInterrupt;
use crate::impl_device_type;
use crate::vstate::memory::GuestMemoryMmap;

#[derive(Debug, thiserror::Error, displaydoc::Display)]
pub enum RdmaError {
    /// Error while handling an Event file descriptor: {0}
    EventFd(#[from] io::Error),
}

#[derive(Debug)]
pub struct VirtioRdma {
    id: String,
    avail_features: u64,
    acked_features: u64,
    activate_event: EventFd,
    device_state: DeviceState,
    queues: Vec<Queue>,
    queue_events: Vec<EventFd>,
}

impl VirtioRdma {
    pub fn new(id: String) -> Result<Self, RdmaError> {
        let activate_event = EventFd::new(libc::EFD_NONBLOCK)?;
        let queues = vec![Queue::new(FIRECRACKER_MAX_QUEUE_SIZE); RDMA_NUM_QUEUES];
        let queue_events = (0..RDMA_NUM_QUEUES)
            .map(|_| EventFd::new(libc::EFD_NONBLOCK))
            .collect::<Result<Vec<EventFd>, io::Error>>()?;

        Ok(Self {
            id,
            avail_features: 0,
            acked_features: 0,
            activate_event,
            device_state: DeviceState::Inactive,
            queues,
            queue_events,
        })
    }

    pub(crate) fn activate_event(&self) -> &EventFd {
        &self.activate_event
    }
}

impl VirtioDevice for VirtioRdma {
    impl_device_type!(VirtioDeviceType::Rdma);

    fn id(&self) -> &str {
        &self.id
    }

    fn queues(&self) -> &[Queue] {
        &self.queues
    }

    fn queues_mut(&mut self) -> &mut [Queue] {
        &mut self.queues
    }

    fn queue_events(&self) -> &[EventFd] {
        &self.queue_events
    }

    fn interrupt_trigger(&self) -> &dyn VirtioInterrupt {
        self.device_state
            .active_state()
            .expect("Device is not initialized")
            .interrupt
            .deref()
    }

    fn avail_features(&self) -> u64 {
        self.avail_features
    }

    fn acked_features(&self) -> u64 {
        self.acked_features
    }

    fn set_acked_features(&mut self, acked_features: u64) {
        self.acked_features = acked_features;
    }

    fn read_config(&self, _offset: u64, _data: &mut [u8]) {}

    fn write_config(&mut self, _offset: u64, _data: &[u8]) {}

    fn is_activated(&self) -> bool {
        self.device_state.is_activated()
    }

    fn activate(
        &mut self,
        mem: GuestMemoryMmap,
        interrupt: Arc<dyn VirtioInterrupt>,
    ) -> Result<(), ActivateError> {
        if self.queues.len() != RDMA_NUM_QUEUES {
            return Err(ActivateError::QueueMismatch {
                expected: RDMA_NUM_QUEUES,
                got: self.queues.len(),
            });
        }

        for q in self.queues.iter_mut() {
            q.initialize(&mem)
                .map_err(ActivateError::QueueMemoryError)?;
        }

        self.activate_event.write(1).map_err(|_| ActivateError::EventFd)?;
        self.device_state = DeviceState::Activated(ActiveState { mem, interrupt });
        Ok(())
    }
}
