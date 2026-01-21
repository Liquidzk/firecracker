// Copyright 2025 Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

use std::collections::VecDeque;
use std::io;
use std::mem::size_of;
use std::ops::Deref;
use std::sync::Arc;

use vmm_sys_util::eventfd::EventFd;

use super::{RDMA_CQ_QUEUE, RDMA_CTRL_QUEUE, RDMA_DATA_QUEUE, RDMA_NUM_QUEUES};
use crate::devices::virtio::ActivateError;
use crate::devices::virtio::device::{ActiveState, DeviceState, VirtioDevice, VirtioDeviceType};
use crate::devices::virtio::generated::virtio_config::VIRTIO_F_VERSION_1;
use crate::devices::virtio::queue::{
    DescriptorChain, FIRECRACKER_MAX_QUEUE_SIZE, InvalidAvailIdx, Queue, QueueError,
};
use crate::devices::virtio::transport::{VirtioInterrupt, VirtioInterruptType};
use crate::impl_device_type;
use crate::logger::{error, info};
use crate::vstate::memory::{ByteValued, Bytes, GuestMemoryMmap};
use vm_memory::{GuestAddress, GuestMemoryError};

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

#[derive(Debug, Clone, Copy)]
enum RdmaQueueKind {
    Ctrl,
    Data,
}

const RDMA_OPCODE_CREATE_QP: u32 = 1;
const RDMA_OPCODE_QUERY_CAPS: u32 = 2;
const RDMA_OPCODE_REGISTER_MR: u32 = 3;
const RDMA_OPCODE_POST_SEND: u32 = 4;
const RDMA_OPCODE_POST_RECV: u32 = 5;
const RDMA_OPCODE_POLL_CQ: u32 = 6;
const RDMA_OPCODE_DESTROY_QP: u32 = 7;
const RDMA_OPCODE_DEREGISTER_MR: u32 = 8;

const RDMA_STATUS_OK: u32 = 0;
const RDMA_STATUS_ERR: u32 = 1;
const RDMA_STATUS_EMPTY: u32 = 2;
const RDMA_DEFAULT_CQ_ID: u32 = 1;

#[derive(Debug, Default, Copy, Clone)]
#[repr(C)]
struct RdmaRequest {
    addr: u64,
    wr_id: u64,
    opcode: u32,
    qp_id: u32,
    mr_id: u32,
    len: u32,
    flags: u32,
    reserved: u32,
}

// SAFETY: RdmaRequest contains only PODs in repr(C) without padding.
unsafe impl ByteValued for RdmaRequest {}

#[derive(Debug, Default, Copy, Clone)]
#[repr(C)]
struct RdmaResponse {
    wr_id: u64,
    status: u32,
    opcode: u32,
    bytes: u32,
    value0: u32,
    value1: u32,
    value2: u32,
    value3: u32,
    value4: u32,
}

// SAFETY: RdmaResponse contains only PODs in repr(C) without padding.
unsafe impl ByteValued for RdmaResponse {}

#[derive(Debug, Default, Copy, Clone)]
#[repr(C)]
struct RdmaCqeWire {
    wr_id: u64,
    status: u32,
    bytes: u32,
    opcode: u32,
    qp_id: u32,
    cq_id: u32,
    reserved: u32,
}

// SAFETY: RdmaCqeWire contains only PODs in repr(C) without padding.
unsafe impl ByteValued for RdmaCqeWire {}

#[derive(Debug, Clone, Copy)]
struct RdmaCaps {
    version_major: u32,
    version_minor: u32,
    max_qp: u32,
    max_mr: u32,
    max_cq: u32,
    max_wr: u32,
}

#[derive(Debug, Clone)]
struct RdmaMr {
    id: u32,
    addr: u64,
    len: u32,
}

#[derive(Debug, Clone)]
struct RdmaWr {
    qp_id: u32,
    mr_id: u32,
    addr: u64,
    len: u32,
    wr_id: u64,
}

#[derive(Debug, Clone)]
struct RdmaCqe {
    wr_id: u64,
    status: u32,
    bytes: u32,
    opcode: u32,
    qp_id: u32,
    cq_id: u32,
}

#[derive(Debug, Clone)]
struct RdmaQp {
    id: u32,
    cq_id: u32,
    pending_sends: VecDeque<RdmaWr>,
    pending_recvs: VecDeque<RdmaWr>,
}

#[derive(Debug, Clone)]
struct RdmaCq {
    id: u32,
    entries: VecDeque<RdmaCqe>,
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
    caps: RdmaCaps,
    next_mr_id: u32,
    mrs: Vec<RdmaMr>,
    qps: Vec<RdmaQp>,
    cqs: Vec<RdmaCq>,
    next_cq_index: usize,
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
            avail_features: 1u64 << VIRTIO_F_VERSION_1,
            acked_features: 0,
            activate_event,
            device_state: DeviceState::Inactive,
            queues,
            queue_events,
            caps: RdmaCaps {
                version_major: 1,
                version_minor: 0,
                max_qp: 8,
                max_mr: 16,
                max_cq: 8,
                max_wr: 128,
            },
            next_mr_id: 1,
            mrs: Vec::new(),
            qps: Vec::new(),
            cqs: Vec::new(),
            next_cq_index: 0,
        })
    }

    pub(crate) fn activate_event(&self) -> &EventFd {
        &self.activate_event
    }

    pub(crate) fn process_ctrl_queue_event(&mut self) {
        if let Err(err) = self.queue_events[RDMA_CTRL_QUEUE].read() {
            error!("rdma: Failed to read ctrl queue event: {err}");
            return;
        }

        self.handle_ctrl_queue().unwrap_or_else(|err| {
            error!("rdma: {err}");
        });
    }

    pub(crate) fn process_data_queue_event(&mut self) {
        if let Err(err) = self.queue_events[RDMA_DATA_QUEUE].read() {
            error!("rdma: Failed to read data queue event: {err}");
            return;
        }

        self.handle_data_queue().unwrap_or_else(|err| {
            error!("rdma: {err}");
        });
    }

    pub(crate) fn process_cq_queue_event(&mut self) {
        if let Err(err) = self.queue_events[RDMA_CQ_QUEUE].read() {
            error!("rdma: Failed to read cq queue event: {err}");
            return;
        }

        self.drain_cq().unwrap_or_else(|err| {
            error!("rdma: {err}");
        });
    }

    fn handle_ctrl_queue(&mut self) -> Result<(), RdmaQueueError> {
        self.handle_queue(RDMA_CTRL_QUEUE, RdmaQueueKind::Ctrl)
    }

    fn handle_data_queue(&mut self) -> Result<(), RdmaQueueError> {
        self.handle_queue(RDMA_DATA_QUEUE, RdmaQueueKind::Data)
    }

    fn handle_queue(
        &mut self,
        queue_index: usize,
        kind: RdmaQueueKind,
    ) -> Result<(), RdmaQueueError> {
        let active_state = self
            .device_state
            .active_state()
            .cloned()
            .expect("Device is not initialized");

        while let Some(head) = self.queues[queue_index].pop()? {
            let used_len = match self.process_chain(&active_state, head, kind) {
                Ok(len) => len,
                Err(err) => {
                    error!("rdma: {err}");
                    0
                }
            };
            if let Err(err) = self.queues[queue_index].add_used(head.index, used_len) {
                error!("rdma: {err}");
                break;
            }
        }

        self.queues[queue_index].advance_used_ring_idx();

        self.drain_cq()?;

        if self.queues[queue_index].prepare_kick() {
            active_state
                .interrupt
                .trigger(VirtioInterruptType::Queue(queue_index as u16))
                .unwrap_or_else(|err| {
                    error!("rdma: Failed to signal queue interrupt: {err:?}");
                });
        }

        Ok(())
    }

    fn process_chain(
        &mut self,
        active_state: &ActiveState,
        head: DescriptorChain,
        kind: RdmaQueueKind,
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
        let mr_id = u32::from_le(request.mr_id);
        let len = u32::from_le(request.len);
        let addr = u64::from_le(request.addr);
        let wr_id = u64::from_le(request.wr_id);
        let cq_id = u32::from_le(request.flags);

        let Some(resp_desc) = head.next_descriptor() else {
            return Err(RdmaQueueError::DescriptorChainTooShort);
        };
        if !resp_desc.is_write_only() {
            return Err(RdmaQueueError::ReadOnlyDescriptor);
        }
        if resp_desc.len < size_of::<RdmaResponse>() as u32 {
            return Err(RdmaQueueError::DescriptorTooShort);
        }

        let mut response = RdmaResponse {
            wr_id: wr_id.to_le(),
            status: RDMA_STATUS_OK.to_le(),
            opcode: opcode.to_le(),
            bytes: 0,
            value0: 0,
            value1: 0,
            value2: 0,
            value3: 0,
            value4: 0,
        };

        match opcode {
            RDMA_OPCODE_POST_SEND | RDMA_OPCODE_POST_RECV => {
                let is_send = opcode == RDMA_OPCODE_POST_SEND;
                let status =
                    self.enqueue_wr(active_state, is_send, qp_id, mr_id, addr, len, wr_id);
                if status != RDMA_STATUS_OK {
                    response.status = status.to_le();
                }
                response.opcode = opcode.to_le();
            }
            _ if matches!(kind, RdmaQueueKind::Data) => {
                response.status = RDMA_STATUS_ERR.to_le();
            }
            RDMA_OPCODE_CREATE_QP => {
                if qp_id == 0
                    || self.qps.iter().any(|qp| qp.id == qp_id)
                    || self.qps.len() >= self.caps.max_qp as usize
                {
                    response.status = RDMA_STATUS_ERR.to_le();
                } else if let Some(resolved_cq) = self.ensure_cq(cq_id) {
                    self.qps.push(RdmaQp {
                        id: qp_id,
                        cq_id: resolved_cq,
                        pending_sends: VecDeque::new(),
                        pending_recvs: VecDeque::new(),
                    });
                    response.value0 = qp_id.to_le();
                    response.value1 = resolved_cq.to_le();
                    info!("virtio-rdma: CREATE_QP qp_id={qp_id} cq_id={resolved_cq}");
                } else {
                    response.status = RDMA_STATUS_ERR.to_le();
                }
            }
            RDMA_OPCODE_QUERY_CAPS => {
                info!("virtio-rdma: QUERY_CAPS");
                response.value0 = self.caps.version_major.to_le();
                response.value1 = self.caps.version_minor.to_le();
                response.value2 = self.caps.max_qp.to_le();
                response.value3 = self.caps.max_mr.to_le();
                response.bytes = self.caps.max_cq.to_le();
                response.value4 = self.caps.max_wr.to_le();
            }
            RDMA_OPCODE_REGISTER_MR => {
                let assigned = if mr_id == 0 {
                    let id = self.next_mr_id;
                    self.next_mr_id = self.next_mr_id.saturating_add(1);
                    id
                } else if self.mrs.iter().any(|mr| mr.id == mr_id) {
                    0
                } else {
                    mr_id
                };

                if assigned == 0 {
                    response.status = RDMA_STATUS_ERR.to_le();
                } else {
                    self.mrs.push(RdmaMr {
                        id: assigned,
                        addr,
                        len,
                    });
                    response.value0 = assigned.to_le();
                    response.opcode = RDMA_OPCODE_REGISTER_MR.to_le();
                    info!(
                        "virtio-rdma: REGISTER_MR id={assigned} addr=0x{addr:x} len={len}"
                    );
                }
            }
            RDMA_OPCODE_POLL_CQ => {
                let cqe = if cq_id == 0 {
                    self.pop_cqe_any()
                } else {
                    self.pop_cqe_from_cq(cq_id)
                };
                if let Some(cqe) = cqe {
                    response.wr_id = cqe.wr_id.to_le();
                    response.status = cqe.status.to_le();
                    response.bytes = cqe.bytes.to_le();
                    response.opcode = cqe.opcode.to_le();
                    response.value0 = cqe.qp_id.to_le();
                    response.value1 = cqe.cq_id.to_le();
                } else {
                    response.status = RDMA_STATUS_EMPTY.to_le();
                }
            }
            RDMA_OPCODE_DESTROY_QP => {
                if let Some(pos) = self.qps.iter().position(|qp| qp.id == qp_id) {
                    let cq_id = self.qps[pos].cq_id;
                    self.qps.remove(pos);
                    self.purge_cq_entries(qp_id, cq_id);
                    if !self.qps.iter().any(|qp| qp.cq_id == cq_id) {
                        self.cqs.retain(|cq| cq.id != cq_id);
                    }
                    info!("virtio-rdma: DESTROY_QP qp_id={qp_id}");
                } else {
                    response.status = RDMA_STATUS_ERR.to_le();
                }
            }
            RDMA_OPCODE_DEREGISTER_MR => {
                let count = self.mrs.len();
                self.mrs.retain(|mr| mr.id != mr_id);
                if self.mrs.len() == count {
                    info!("virtio-rdma: DEREGISTER_MR id={mr_id} (not found)");
                } else {
                    info!("virtio-rdma: DEREGISTER_MR id={mr_id}");
                }
            }
            _ => {
                response.status = RDMA_STATUS_ERR.to_le();
            }
        }

        active_state.mem.write_obj(response, resp_desc.addr)?;

        Ok(size_of::<RdmaResponse>() as u32)
    }

    fn drain_cq(&mut self) -> Result<(), RdmaQueueError> {
        let active_state = self
            .device_state
            .active_state()
            .cloned()
            .expect("Device is not initialized");
        let mut used_any = false;

        while self.has_cqe() && !self.queues[RDMA_CQ_QUEUE].is_empty() {
            let Some(head) = self.queues[RDMA_CQ_QUEUE].pop()? else {
                break;
            };
            if !head.is_write_only() {
                return Err(RdmaQueueError::ReadOnlyDescriptor);
            }
            if head.len < size_of::<RdmaCqeWire>() as u32 {
                return Err(RdmaQueueError::DescriptorTooShort);
            }

            let Some(cqe) = self.pop_cqe_any() else {
                break;
            };
            let wire = RdmaCqeWire {
                wr_id: cqe.wr_id.to_le(),
                status: cqe.status.to_le(),
                bytes: cqe.bytes.to_le(),
                opcode: cqe.opcode.to_le(),
                qp_id: cqe.qp_id.to_le(),
                cq_id: cqe.cq_id.to_le(),
                reserved: 0,
            };
            active_state.mem.write_obj(wire, head.addr)?;
            self.queues[RDMA_CQ_QUEUE].add_used(head.index, size_of::<RdmaCqeWire>() as u32)?;
            used_any = true;
        }

        if used_any {
            self.queues[RDMA_CQ_QUEUE].advance_used_ring_idx();
            if self.queues[RDMA_CQ_QUEUE].prepare_kick() {
                active_state
                    .interrupt
                    .trigger(VirtioInterruptType::Queue(RDMA_CQ_QUEUE as u16))
                    .unwrap_or_else(|err| {
                        error!("rdma: Failed to signal cq interrupt: {err:?}");
                    });
            }
        }

        Ok(())
    }

    fn ensure_cq(&mut self, cq_id: u32) -> Option<u32> {
        let resolved = if cq_id == 0 { RDMA_DEFAULT_CQ_ID } else { cq_id };
        if self.cqs.iter().any(|cq| cq.id == resolved) {
            return Some(resolved);
        }
        if self.cqs.len() >= self.caps.max_cq as usize {
            return None;
        }
        self.cqs.push(RdmaCq {
            id: resolved,
            entries: VecDeque::new(),
        });
        Some(resolved)
    }

    fn has_cqe(&self) -> bool {
        self.cqs.iter().any(|cq| !cq.entries.is_empty())
    }

    fn total_cqe_len(&self) -> usize {
        self.cqs.iter().map(|cq| cq.entries.len()).sum()
    }

    fn total_pending_len(&self) -> usize {
        self.qps
            .iter()
            .map(|qp| qp.pending_sends.len() + qp.pending_recvs.len())
            .sum()
    }

    fn pop_cqe_from_cq(&mut self, cq_id: u32) -> Option<RdmaCqe> {
        self.cqs
            .iter_mut()
            .find(|cq| cq.id == cq_id)
            .and_then(|cq| cq.entries.pop_front())
    }

    fn pop_cqe_any(&mut self) -> Option<RdmaCqe> {
        if self.cqs.is_empty() {
            return None;
        }
        let start = self.next_cq_index % self.cqs.len();
        for offset in 0..self.cqs.len() {
            let idx = (start + offset) % self.cqs.len();
            if let Some(cqe) = self.cqs[idx].entries.pop_front() {
                self.next_cq_index = (idx + 1) % self.cqs.len();
                return Some(cqe);
            }
        }
        None
    }

    fn purge_cq_entries(&mut self, qp_id: u32, cq_id: u32) {
        let Some(cq) = self.cqs.iter_mut().find(|cq| cq.id == cq_id) else {
            return;
        };
        cq.entries.retain(|cqe| cqe.qp_id != qp_id);
    }

    fn enqueue_wr(
        &mut self,
        active_state: &ActiveState,
        is_send: bool,
        qp_id: u32,
        mr_id: u32,
        addr: u64,
        len: u32,
        wr_id: u64,
    ) -> u32 {
        let mr = match self.mrs.iter().find(|mr| mr.id == mr_id) {
            Some(mr) => mr,
            None => return RDMA_STATUS_ERR,
        };

        let qp_index = match self.qps.iter().position(|qp| qp.id == qp_id) {
            Some(index) => index,
            None => return RDMA_STATUS_ERR,
        };

        let effective_addr = if addr == 0 { mr.addr } else { addr };
        let effective_len = if len == 0 { mr.len } else { len.min(mr.len) };

        let wr = RdmaWr {
            qp_id,
            mr_id,
            addr: effective_addr,
            len: effective_len,
            wr_id,
        };

        if self.total_pending_len() >= self.caps.max_wr as usize {
            return RDMA_STATUS_ERR;
        }

        if is_send {
            info!(
                "virtio-rdma: POST_SEND qp_id={qp_id} mr_id={mr_id} len={effective_len}"
            );
            self.qps[qp_index].pending_sends.push_back(wr);
        } else {
            info!(
                "virtio-rdma: POST_RECV qp_id={qp_id} mr_id={mr_id} len={effective_len}"
            );
            self.qps[qp_index].pending_recvs.push_back(wr);
        }

        self.match_send_recv(active_state, qp_id);
        RDMA_STATUS_OK
    }

    fn match_send_recv(&mut self, active_state: &ActiveState, qp_id: u32) {
        let mut completions = Vec::new();
        let cq_id;

        {
            let Some(qp) = self.qps.iter_mut().find(|qp| qp.id == qp_id) else {
                return;
            };
            cq_id = qp.cq_id;

            while !qp.pending_sends.is_empty() && !qp.pending_recvs.is_empty() {
                let send = qp.pending_sends.pop_front().expect("pending_send missing");
                let recv = qp.pending_recvs.pop_front().expect("pending_recv missing");

                let bytes = send.len.min(recv.len);
                let mut status = RDMA_STATUS_OK;
                if bytes > 0 && send.addr != 0 && recv.addr != 0 {
                    let mut buf = vec![0u8; bytes as usize];
                    if let Err(err) = active_state
                        .mem
                        .read_slice(&mut buf, GuestAddress(send.addr))
                    {
                        error!("rdma: Failed to read send buffer: {err}");
                        status = RDMA_STATUS_ERR;
                    } else if let Err(err) = active_state
                        .mem
                        .write_slice(&buf, GuestAddress(recv.addr))
                    {
                        error!("rdma: Failed to write recv buffer: {err}");
                        status = RDMA_STATUS_ERR;
                    }
                }

                completions.push(RdmaCqe {
                    wr_id: send.wr_id,
                    status,
                    bytes,
                    opcode: RDMA_OPCODE_POST_SEND,
                    qp_id,
                    cq_id,
                });
                completions.push(RdmaCqe {
                    wr_id: recv.wr_id,
                    status,
                    bytes,
                    opcode: RDMA_OPCODE_POST_RECV,
                    qp_id,
                    cq_id,
                });
            }
        }

        if completions.is_empty() {
            return;
        }

        if self.total_cqe_len() + completions.len() > (self.caps.max_wr as usize) * 2 {
            error!("rdma: CQ overflow");
            return;
        }

        let Some(cq) = self.cqs.iter_mut().find(|cq| cq.id == cq_id) else {
            error!("rdma: CQ id {cq_id} not found");
            return;
        };
        for cqe in completions {
            cq.entries.push_back(cqe);
        }

        if let Err(err) = self.drain_cq() {
            error!("rdma: {err}");
        }
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

    fn reset(&mut self) -> Option<(Arc<dyn VirtioInterrupt>, Vec<EventFd>)> {
        let interrupt = self
            .device_state
            .active_state()
            .map(|state| state.interrupt.clone());
        self.device_state = DeviceState::Inactive;
        self.acked_features = 0;
        self.qps.clear();
        self.cqs.clear();
        self.next_cq_index = 0;
        self.mrs.clear();
        interrupt.map(|intr| (intr, Vec::new()))
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
            addr: 0,
            wr_id: 0,
            opcode: RDMA_OPCODE_CREATE_QP.to_le(),
            qp_id: 7u32.to_le(),
            mr_id: 0,
            len: 0,
            flags: 0,
            reserved: 0,
        };
        mem.write_obj(request, GuestAddress(req_addr)).unwrap();
        mem.write_obj(
            RdmaResponse {
                wr_id: 0,
                status: 0xdead_beef,
                opcode: 0,
                bytes: 0,
                value0: 0,
                value1: 0,
                value2: 0,
                value3: 0,
                value4: 0,
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

    #[test]
    fn test_rdma_query_caps() {
        let mem = default_mem();
        let device = VirtioRdma::new("rdma0".to_string()).unwrap();
        let mut th = VirtioTestHelper::<VirtioRdma>::new(&mem, device);
        th.activate_device(&mem);

        let req_addr = th.data_address() + 0x300;
        let resp_addr = th.data_address() + 0x400;
        let request = RdmaRequest {
            addr: 0,
            wr_id: 0,
            opcode: RDMA_OPCODE_QUERY_CAPS.to_le(),
            qp_id: 0,
            mr_id: 0,
            len: 0,
            flags: 0,
            reserved: 0,
        };
        mem.write_obj(request, GuestAddress(req_addr)).unwrap();
        mem.write_obj(
            RdmaResponse {
                wr_id: 0,
                status: 0,
                opcode: 0,
                bytes: 0,
                value0: 0,
                value1: 0,
                value2: 0,
                value3: 0,
                value4: 0,
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
        assert_eq!(u32::from_le(response.opcode), RDMA_OPCODE_QUERY_CAPS);
        assert_eq!(u32::from_le(response.value0), 1);
        assert_eq!(u32::from_le(response.value1), 0);
        assert_eq!(u32::from_le(response.value2), 8);
        assert_eq!(u32::from_le(response.value3), 16);
        assert_eq!(u32::from_le(response.bytes), 8);
        assert_eq!(u32::from_le(response.value4), 128);
    }
}
