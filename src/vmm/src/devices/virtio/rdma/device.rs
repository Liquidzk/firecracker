// Copyright 2025 Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

use std::io;
use std::mem::size_of;
use std::ops::Deref;
use std::sync::Arc;

use vmm_sys_util::eventfd::EventFd;

use super::RDMA_NUM_QUEUES;
use crate::devices::virtio::ActivateError;
use crate::devices::virtio::device::{ActiveState, DeviceState, VirtioDevice, VirtioDeviceType};
use crate::devices::virtio::queue::{
    DescriptorChain, FIRECRACKER_MAX_QUEUE_SIZE, InvalidAvailIdx, Queue, QueueError,
};
use crate::devices::virtio::transport::{VirtioInterrupt, VirtioInterruptType};
use crate::impl_device_type;
use crate::logger::{error, info};
use crate::vstate::memory::{ByteValued, Bytes, GuestMemoryMmap};
use vm_memory::GuestMemoryError;

#[derive(Debug, thiserror::Error, displaydoc::Display)]
pub enum RdmaError {
    /// Error while handling an Event file descriptor: {0}
    EventFd(#[from] io::Error),
}

#[derive(Debug, thiserror::Error, displaydoc::Display)]
enum RdmaQueueError {
    /// Unexpected write-only descriptor
    WriteOnlyDescriptor,
    /// Unexpected read-only descriptor
    ReadOnlyDescriptor,
    /// Descriptor chain too short
    DescriptorChainTooShort,
    /// Descriptor length too small
    DescriptorTooShort,
    /// Guest memory error: {0}
    GuestMemory(#[from] GuestMemoryError),
    /// Error handling the VirtIO queue: {0}
    Queue(#[from] QueueError),
    /// Error obtaining a descriptor from the queue: {0}
    QueuePop(#[from] InvalidAvailIdx),
}

const RDMA_OPCODE_CREATE_QP: u32 = 1;
const RDMA_STATUS_OK: u32 = 0;
const RDMA_STATUS_ERR: u32 = 1;

#[derive(Debug, Default, Copy, Clone)]
#[repr(C)]
struct RdmaRequest {
    opcode: u32,
    qp_id: u32,
}

// SAFETY: RdmaRequest contains only PODs in repr(C) without padding.
unsafe impl ByteValued for RdmaRequest {}

#[derive(Debug, Default, Copy, Clone)]
#[repr(C)]
struct RdmaResponse {
    status: u32,
}

// SAFETY: RdmaResponse contains only PODs in repr(C) without padding.
unsafe impl ByteValued for RdmaResponse {}

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

    pub(crate) fn process_queue_event(&mut self) {
        if let Err(err) = self.queue_events[0].read() {
            error!("rdma: Failed to read queue event: {err}");
            return;
        }

        self.handle_queue().unwrap_or_else(|err| {
            error!("rdma: {err}");
        });
    }

    fn handle_queue(&mut self) -> Result<(), RdmaQueueError> {
        let active_state = self
            .device_state
            .active_state()
            .cloned()
            .expect("Device is not initialized");

        while let Some(head) = self.queues[0].pop()? {
            let used_len = match self.process_chain(&active_state, head) {
                Ok(len) => len,
                Err(err) => {
                    error!("rdma: {err}");
                    0
                }
            };
            if let Err(err) = self.queues[0].add_used(head.index, used_len) {
                error!("rdma: {err}");
                break;
            }
        }

        self.queues[0].advance_used_ring_idx();

        if self.queues[0].prepare_kick() {
            active_state
                .interrupt
                .trigger(VirtioInterruptType::Queue(0))
                .unwrap_or_else(|err| {
                    error!("rdma: Failed to signal queue interrupt: {err:?}");
                });
        }

        Ok(())
    }

    fn process_chain(
        &self,
        active_state: &ActiveState,
        head: DescriptorChain,
    ) -> Result<u32, RdmaQueueError> {
        if head.is_write_only() {
            return Err(RdmaQueueError::WriteOnlyDescriptor);
        }
        if head.len < size_of::<RdmaRequest>() as u32 {
            return Err(RdmaQueueError::DescriptorTooShort);
        }

        let request: RdmaRequest = active_state.mem.read_obj(head.addr)?;
        let opcode = u32::from_le(request.opcode);
        let qp_id = u32::from_le(request.qp_id);

        let Some(resp_desc) = head.next_descriptor() else {
            return Err(RdmaQueueError::DescriptorChainTooShort);
        };
        if !resp_desc.is_write_only() {
            return Err(RdmaQueueError::ReadOnlyDescriptor);
        }
        if resp_desc.len < size_of::<RdmaResponse>() as u32 {
            return Err(RdmaQueueError::DescriptorTooShort);
        }

        let status = if opcode == RDMA_OPCODE_CREATE_QP {
            info!("virtio-rdma: CREATE_QP qp_id={qp_id}");
            RDMA_STATUS_OK
        } else {
            RDMA_STATUS_ERR
        };

        let response = RdmaResponse {
            status: status.to_le(),
        };
        active_state.mem.write_obj(response, resp_desc.addr)?;

        Ok(size_of::<RdmaResponse>() as u32)
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::devices::virtio::queue::VIRTQ_DESC_F_WRITE;
    use crate::devices::virtio::test_utils::default_mem;
    use crate::devices::virtio::test_utils::test::{VirtioTestDevice, VirtioTestHelper};
    use crate::vstate::memory::{Bytes, GuestAddress};

    impl VirtioTestDevice for VirtioRdma {
        fn set_queues(&mut self, queues: Vec<Queue>) {
            self.queues = queues;
        }

        fn num_queues(&self) -> usize {
            RDMA_NUM_QUEUES
        }
    }

    #[test]
    fn test_rdma_create_qp() {
        let mem = default_mem();
        let device = VirtioRdma::new("rdma0".to_string()).unwrap();
        let mut th = VirtioTestHelper::<VirtioRdma>::new(&mem, device);
        th.activate_device(&mem);

        let req_addr = th.data_address() + 0x100;
        let resp_addr = th.data_address() + 0x200;
        let request = RdmaRequest {
            opcode: RDMA_OPCODE_CREATE_QP.to_le(),
            qp_id: 7u32.to_le(),
        };
        mem.write_obj(request, GuestAddress(req_addr)).unwrap();
        mem.write_obj(
            RdmaResponse {
                status: 0xdead_beef,
            },
            GuestAddress(resp_addr),
        )
        .unwrap();

        th.add_scatter_gather(
            0,
            0,
            &[
                (0, req_addr, size_of::<RdmaRequest>() as u32, 0),
                (
                    1,
                    resp_addr,
                    size_of::<RdmaResponse>() as u32,
                    VIRTQ_DESC_F_WRITE,
                ),
            ],
        );

        th.emulate_for_msec(100).unwrap();

        let response: RdmaResponse = mem.read_obj(GuestAddress(resp_addr)).unwrap();
        assert_eq!(u32::from_le(response.status), RDMA_STATUS_OK);

        assert_eq!(th.device().queues[0].next_used.0, 1);
    }
}
