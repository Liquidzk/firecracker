// Copyright 2025 Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

use std::sync::{Arc, Mutex};

use serde::{Deserialize, Serialize};

use crate::devices::virtio::device::VirtioDevice;
use crate::devices::virtio::rdma::{RdmaError, VirtioRdma};

/// Use this structure to set up an RDMA device before booting the kernel.
#[derive(Debug, Default, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RdmaDeviceConfig {
    /// Unique identifier of the device.
    pub id: String,
}

impl From<&VirtioRdma> for RdmaDeviceConfig {
    fn from(device: &VirtioRdma) -> Self {
        RdmaDeviceConfig {
            id: device.id().to_string(),
        }
    }
}

/// Errors associated with the operations allowed on an RDMA device.
#[derive(Debug, thiserror::Error, displaydoc::Display)]
pub enum RdmaDeviceError {
    /// Unable to create the virtio-rdma device: {0}
    CreateDevice(#[from] RdmaError),
}

/// Builder for a list of RDMA devices.
#[derive(Debug, Default)]
pub struct RdmaDeviceBuilder {
    devices: Vec<Arc<Mutex<VirtioRdma>>>,
}

impl RdmaDeviceBuilder {
    /// Creates an empty list of RDMA devices.
    pub fn new() -> Self {
        RdmaDeviceBuilder {
            devices: Vec::new(),
        }
    }

    /// Returns an immutable iterator over the RDMA devices.
    pub fn iter(&self) -> ::std::slice::Iter<'_, Arc<Mutex<VirtioRdma>>> {
        self.devices.iter()
    }

    /// Adds an existing RDMA device in the builder.
    pub fn add_device(&mut self, device: Arc<Mutex<VirtioRdma>>) {
        self.devices.push(device);
    }

    /// Builds an RDMA device based on a configuration and keeps a reference in the list.
    pub fn build(
        &mut self,
        config: RdmaDeviceConfig,
    ) -> Result<Arc<Mutex<VirtioRdma>>, RdmaDeviceError> {
        let id = config.id.clone();
        let position = self
            .devices
            .iter()
            .position(|dev| dev.lock().expect("Poisoned lock").id() == config.id);
        let device = Arc::new(Mutex::new(VirtioRdma::new(id)?));

        if let Some(index) = position {
            self.devices[index] = device.clone();
        } else {
            self.devices.push(device.clone());
        }

        Ok(device)
    }

    /// Inserts a new RDMA device from a configuration object.
    pub fn insert(&mut self, config: RdmaDeviceConfig) -> Result<(), RdmaDeviceError> {
        let _ = self.build(config)?;
        Ok(())
    }

    /// Returns a vec with the structures used to configure the devices.
    pub fn configs(&self) -> Vec<RdmaDeviceConfig> {
        self.devices
            .iter()
            .map(|device| RdmaDeviceConfig::from(&*device.lock().expect("Poisoned lock")))
            .collect()
    }
}
