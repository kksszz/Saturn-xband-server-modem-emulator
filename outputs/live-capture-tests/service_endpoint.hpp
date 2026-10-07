#pragma once
// Compatibility bridge: one implementation, independently buildable from Ymir.
#include "../../components/xband/include/xband/service_endpoint.hpp"
using ServiceEndpoint = xband::ServiceEndpoint;
using xband::pumpService;
