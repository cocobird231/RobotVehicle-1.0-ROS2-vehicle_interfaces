#include <rclcpp/rclcpp.hpp>
#include "params.h"
#include "qos.h"
#include "safety.h"
#include "timesync.h"

namespace vehicle_interfaces
{

class VehicleServiceNode : public QoSUpdateNode, public SafetyNode, public TimeSyncNode
{
public:
    VehicleServiceNode(const std::shared_ptr<GenericParams>& gParams) : 
        QoSUpdateNode(gParams->nodeName, gParams->qosService), 
        SafetyNode(gParams->nodeName, gParams->safetyService), 
        TimeSyncNode(gParams->nodeName, gParams->timesyncService, gParams->timesyncInterval_ms, gParams->timesyncAccuracy_ms), 
        rclcpp::Node(gParams->nodeName) {}
};

}

