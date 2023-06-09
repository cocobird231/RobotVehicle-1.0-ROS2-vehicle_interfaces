#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <mutex>

#include "rclcpp/rclcpp.hpp"
#include "vehicle_interfaces/msg/qos_update.hpp"
#include "vehicle_interfaces/srv/qos_req.hpp"

using namespace std::chrono_literals;
using namespace std::placeholders;

namespace vehicle_interfaces
{

/* The QoSUpdateNode class implements the QoS update mechanisms, including a subscription of QoS update topic and a client for QoS update service.
 * The publish or subscription node can easily inherit the QoSUpdateNode, and adding callback function with addQoSCallbackFunc() to get callback 
 * while QoS policy updated. 
 * The QoS update mechanisms relying on the name of topics, so it's necessary to add topic names to QoSUpdateNode by calling addQoSTracking() to 
 * tracks whether the QoS of specific topic needs to be update.
 */
class QoSUpdateNode : virtual public rclcpp::Node
{
private:
    // QoS Service Definition
    std::shared_ptr<rclcpp::Node> reqClientNode_;// QOS service node
    rclcpp::Client<vehicle_interfaces::srv::QosReq>::SharedPtr reqClient_;// QoS service client

    // QoS Topic Definition
    rclcpp::Subscription<vehicle_interfaces::msg::QosUpdate>::SharedPtr subscription_;// QoS subscription
    std::atomic<uint64_t> qosID_;// Unique ID that describes current QoS' profile
    std::vector<std::string> qosTopicNameVec_;// Topics QoS need to be tracked
    std::mutex subscriptionLock_;// Prevent QoS callback conflict

    // QoS Changed Callback
    std::atomic<bool> callbackF_;
    std::function<void(QoSUpdateNode*, std::map<std::string, rclcpp::QoS*>)> qosCallbackFunc_;

    // Node enable
    std::atomic<bool> nodeEnableF_;

private:
    void _topic_callback(const vehicle_interfaces::msg::QosUpdate::SharedPtr msg)
    {
        if (!this->callbackF_)// No callback function assigned
            return;
        
        if (msg->qid == this->qosID_)// Ignore update in same qos ID
            return;

        bool errF = false;
        
        std::map<std::string, rclcpp::QoS*> qmap;

        std::unique_lock<std::mutex> locker(this->subscriptionLock_, std::defer_lock);
        locker.lock();
        for (auto& myTopic : this->qosTopicNameVec_)
        {
            for (auto& newTopic : msg->topic_table)
            {
                if (myTopic == newTopic)
                {
                    try
                    {
                        qmap[myTopic] = this->requestQoS(myTopic);
                    }
                    catch(...)
                    {
                        errF = true;
                    }
                    break;
                }
            }
        }
        locker.unlock();

        if (errF)
            return;
        
        if (qmap.size() > 0)
            this->qosCallbackFunc_(this, qmap);
        
        this->qosID_ = msg->qid;
    }

    void _connToService(rclcpp::ClientBase::SharedPtr client)
    {
        int errCnt = 5;
        while (!client->wait_for_service(1s) && errCnt-- > 0)
        {
            if (!rclcpp::ok())
            {
                RCLCPP_ERROR(this->get_logger(), "[QoSUpdateNode::_connToService] Interrupted while waiting for the service. Exiting.");
                return;
            }
            RCLCPP_INFO(this->get_logger(), "[QoSUpdateNode::_connToService] Service not available, waiting again...");
        }
        if (errCnt < 0)
            RCLCPP_ERROR(this->get_logger(), "[QoSUpdateNode::_connToService] Connect to service failed.");
        else
            RCLCPP_INFO(this->get_logger(), "[QoSUpdateNode::_connToService] Service connected.");
    }

    rmw_time_t _splitTime(const double& time_ms)
    {
        return { time_ms / 1000, (time_ms - (uint64_t)time_ms) * 1000000 };
    }

public:
    QoSUpdateNode(std::string nodeName, std::string qosServiceName) : rclcpp::Node(nodeName), 
        qosID_(0), 
        callbackF_(false), 
        nodeEnableF_(false)
    {
        if (qosServiceName == "")
            return;
        this->reqClientNode_ = rclcpp::Node::make_shared(nodeName + "_qosreq_client");
        this->reqClient_ = this->reqClientNode_->create_client<vehicle_interfaces::srv::QosReq>(qosServiceName + "_Req");
        printf("[QoSUpdateNode] Connecting to qosreq server: %s\n", qosServiceName.c_str());
        this->_connToService(this->reqClient_);

        this->subscription_ = this->create_subscription<vehicle_interfaces::msg::QosUpdate>(qosServiceName, 
            10, std::bind(&QoSUpdateNode::_topic_callback, this, std::placeholders::_1));
        this->nodeEnableF_ = true;
    }

    void addQoSTracking(const std::string& topicName)
    {
        if (!this->nodeEnableF_)
            return;
        std::lock_guard<std::mutex> locker(this->subscriptionLock_);
        for (auto& i : this->qosTopicNameVec_)
            if (i == topicName)
                return;
        this->qosTopicNameVec_.push_back(topicName);
        this->qosID_ = 0;
    }

    void addQoSCallbackFunc(const std::function<void(QoSUpdateNode*, std::map<std::string, rclcpp::QoS*>)>& func)
    {
        if (!this->nodeEnableF_)
            return;
        std::lock_guard<std::mutex> locker(this->subscriptionLock_);
        this->qosCallbackFunc_ = func;
        this->callbackF_ = true;
    }

    rclcpp::QoS* requestQoS(const std::string& topicName)
    {
        if (!this->nodeEnableF_)
            throw "Request QoS Failed";// Request QoS failed
        auto request = std::make_shared<vehicle_interfaces::srv::QosReq::Request>();
        request->topic_name = topicName;
        auto result = this->reqClient_->async_send_request(request);
#if ROS_DISTRO == 0
        if (rclcpp::spin_until_future_complete(this->reqClientNode_, result, 10ms) == rclcpp::executor::FutureReturnCode::SUCCESS)
#else
        if (rclcpp::spin_until_future_complete(this->reqClientNode_, result, 10ms) == rclcpp::FutureReturnCode::SUCCESS)
#endif
        {
            auto res = result.get();
            if (res->response)
            {
                std::lock_guard<std::mutex> locker(this->subscriptionLock_);

                rmw_qos_profile_t prof;
                prof.history = (rmw_qos_history_policy_t)res->history;
                prof.depth = res->depth;
                prof.reliability = (rmw_qos_reliability_policy_t)res->reliability;
                prof.durability = (rmw_qos_durability_policy_t)res->durability;
                prof.deadline = this->_splitTime(res->deadline_ms);
                prof.lifespan = this->_splitTime(res->lifespan_ms);
                prof.liveliness = (rmw_qos_liveliness_policy_t)res->liveliness;
                prof.liveliness_lease_duration = this->_splitTime(res->liveliness_lease_duration_ms);

                // this->qosID_ = res->qid;
                return new rclcpp::QoS(rclcpp::QoSInitialization(prof.history, prof.depth), prof);
            }
        }
        throw "Request QoS Failed";// Request QoS failed
    }
};

}// namespace vehicle_interfaces
