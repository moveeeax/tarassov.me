/**
 * @file Messaging.hpp
 * @brief Messaging module for Kafka integration
 * @details Provides Kafka producer and consumer using librdkafka
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <librdkafka/rdkafkacpp.h>
#include <spdlog/spdlog.h>

namespace Messaging {

using namespace std::chrono_literals;

namespace detail {
/**
 * @brief Delivery-report callback: produce() only enqueues a message; the
 *        broker ack (or a permanent send failure after retries) is reported
 *        here asynchronously. Without this, failed deliveries are invisible.
 */
class ProducerDeliveryReportCb : public RdKafka::DeliveryReportCb {
public:
    void dr_cb(RdKafka::Message& message) override {
        if (message.err() != RdKafka::ERR_NO_ERROR) {
            spdlog::error("Kafka delivery FAILED (topic={}): {}", message.topic_name(), message.errstr());
        } else {
            spdlog::debug("Kafka message delivered (topic={}, partition={}, offset={})",
                          message.topic_name(),
                          message.partition(),
                          message.offset());
        }
    }
};

/// Process-lifetime callback instance — the producer's Conf holds a pointer to
/// it, so it must outlive the producer.
inline ProducerDeliveryReportCb& producer_dr_cb() {
    static ProducerDeliveryReportCb cb;
    return cb;
}
}  // namespace detail

/**
 * @brief Kafka producer wrapper
 */
class KafkaProducer {
private:
    std::unique_ptr<RdKafka::Producer> producer;
    std::unique_ptr<RdKafka::Conf> conf;
    std::string brokers;
    bool initialized = false;

public:
    void initialize(const std::string& broker_list, const std::string& client_id = "cpp_producer") {
        if (initialized) {
            throw std::runtime_error("Kafka producer already initialized");
        }

        brokers = broker_list;
        conf.reset(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));

        std::string errstr;

        if (conf->set("metadata.broker.list", brokers, errstr) != RdKafka::Conf::CONF_OK) {
            throw std::runtime_error("Failed to set broker list: " + errstr);
        }

        if (conf->set("client.id", client_id, errstr) != RdKafka::Conf::CONF_OK) {
            throw std::runtime_error("Failed to set client ID: " + errstr);
        }

        // Reliability: the idempotent producer guarantees no duplicates / no
        // reordering across retries and implies acks=all (wait for all in-sync
        // replicas). Without it, produce() is fire-and-forget and a transient
        // broker error silently drops the message.
        if (conf->set("enable.idempotence", "true", errstr) != RdKafka::Conf::CONF_OK) {
            throw std::runtime_error("Failed to set enable.idempotence: " + errstr);
        }
        // Surface async delivery failures (see ProducerDeliveryReportCb).
        if (conf->set("dr_cb", &detail::producer_dr_cb(), errstr) != RdKafka::Conf::CONF_OK) {
            throw std::runtime_error("Failed to set dr_cb: " + errstr);
        }

        producer.reset(RdKafka::Producer::create(conf.get(), errstr));
        if (!producer) {
            throw std::runtime_error("Failed to create producer: " + errstr);
        }

        initialized = true;
        spdlog::info("Kafka producer initialized (brokers: {})", brokers);
    }

    bool produce(const std::string& topic,
                 const std::string& key,
                 const std::string& payload,
                 int partition = RdKafka::Topic::PARTITION_UA) {
        check_initialized();

        RdKafka::ErrorCode resp = producer->produce(topic,
                                                    partition,
                                                    RdKafka::Producer::RK_MSG_COPY,
                                                    const_cast<char*>(payload.c_str()),
                                                    payload.size(),
                                                    key.empty() ? nullptr : key.c_str(),
                                                    key.size(),
                                                    0,
                                                    nullptr);

        if (resp != RdKafka::ERR_NO_ERROR) {
            spdlog::error("Failed to produce message: {}", RdKafka::err2str(resp));
            return false;
        }

        producer->poll(0);
        return true;
    }

    bool produce(const std::string& topic, const std::string& payload) { return produce(topic, "", payload); }

    int flush(int timeout_ms = 10000) {
        check_initialized();
        return producer->flush(timeout_ms);
    }

    void poll(int timeout_ms = 0) {
        check_initialized();
        producer->poll(timeout_ms);
    }

    int outq_len() {
        check_initialized();
        return producer->outq_len();
    }

    void shutdown() {
        if (initialized) {
            spdlog::info("Shutting down Kafka producer");
            int unsent = flush(5000);
            if (unsent > 0)
                spdlog::warn("Kafka producer shutdown: {} message(s) still undelivered after flush timeout", unsent);
            producer.reset();
            conf.reset();
            initialized = false;
        }
    }

    bool is_initialized() const { return initialized; }

private:
    void check_initialized() const {
        if (!initialized) {
            throw std::runtime_error("Kafka producer not initialized");
        }
    }
};

/**
 * @brief Kafka consumer wrapper
 */
class KafkaConsumer {
private:
    std::unique_ptr<RdKafka::KafkaConsumer> consumer;
    std::unique_ptr<RdKafka::Conf> conf;
    std::vector<std::string> topics;
    std::string brokers;
    bool initialized = false;
    // Written by stop_consuming() on one thread, polled by the loop on
    // another — must be atomic, or the compiler may hoist the read out of the
    // while(consuming) loop and stop never takes effect.
    std::atomic<bool> consuming{false};

    // Poison-message bound: a permanently-failing message left uncommitted would
    // redeliver forever and pin the partition. Track the last failing offset and
    // skip past it after kMaxPoisonAttempts (a real consumer would dead-letter
    // it). Single-offset tracking is naive but enough for the template.
    static constexpr int kMaxPoisonAttempts = 5;
    int64_t poison_offset_ = -1;
    int poison_count_ = 0;

public:
    void initialize(const std::string& broker_list,
                    const std::string& group_id,
                    const std::vector<std::string>& topic_list,
                    const std::string& auto_offset_reset = "earliest") {
        if (initialized) {
            throw std::runtime_error("Kafka consumer already initialized");
        }

        brokers = broker_list;
        topics = topic_list;
        conf.reset(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));

        std::string errstr;

        if (conf->set("metadata.broker.list", brokers, errstr) != RdKafka::Conf::CONF_OK)
            throw std::runtime_error("Failed to set broker list: " + errstr);
        if (conf->set("group.id", group_id, errstr) != RdKafka::Conf::CONF_OK)
            throw std::runtime_error("Failed to set group ID: " + errstr);
        if (conf->set("auto.offset.reset", auto_offset_reset, errstr) != RdKafka::Conf::CONF_OK)
            throw std::runtime_error("Failed to set auto.offset.reset: " + errstr);
        // Manual commit only: auto-commit acks offsets on a timer regardless of
        // whether the callback succeeded, which silently drops a message whose
        // processing threw (at-most-once). We commit explicitly AFTER the
        // callback returns so a failure redelivers (at-least-once).
        if (conf->set("enable.auto.commit", "false", errstr) != RdKafka::Conf::CONF_OK)
            throw std::runtime_error("Failed to set auto.commit: " + errstr);

        consumer.reset(RdKafka::KafkaConsumer::create(conf.get(), errstr));
        if (!consumer) {
            throw std::runtime_error("Failed to create consumer: " + errstr);
        }

        RdKafka::ErrorCode err = consumer->subscribe(topics);
        if (err != RdKafka::ERR_NO_ERROR) {
            throw std::runtime_error("Failed to subscribe: " + RdKafka::err2str(err));
        }

        initialized = true;
        spdlog::info("Kafka consumer initialized (group: {}, topics: {})", group_id, topics.size());
    }

    std::unique_ptr<RdKafka::Message> consume(int timeout_ms = 1000) {
        check_initialized();

        std::unique_ptr<RdKafka::Message> msg(consumer->consume(timeout_ms));

        if (msg->err() == RdKafka::ERR_NO_ERROR) {
            return msg;
        } else if (msg->err() == RdKafka::ERR__PARTITION_EOF || msg->err() == RdKafka::ERR__TIMED_OUT) {
            return nullptr;
        } else {
            spdlog::error("Consume error: {}", msg->errstr());
            return nullptr;
        }
    }

    void start_consuming(std::function<void(const std::string&, const std::string&)> callback, int timeout_ms = 1000) {
        check_initialized();
        consuming = true;

        while (consuming) {
            auto msg = consume(timeout_ms);
            if (msg && msg->err() == RdKafka::ERR_NO_ERROR) {
                std::string key = msg->key() ? *msg->key() : "";
                std::string payload(static_cast<const char*>(msg->payload()), static_cast<size_t>(msg->len()));
                bool ok = false;
                try {
                    callback(key, payload);
                    ok = true;
                } catch (const std::exception& e) {
                    spdlog::error("Error in message callback (offset NOT committed, will redeliver): {}", e.what());
                } catch (...) {
                    // Don't let a non-std throw escape and kill the consumer thread.
                    spdlog::error("Unknown (non-std) error in message callback — offset not committed");
                }

                if (ok) {
                    // Success → commit (at-least-once). Reset the poison tracker.
                    poison_offset_ = -1;
                    poison_count_ = 0;
                    RdKafka::ErrorCode cerr = consumer->commitSync(msg.get());
                    if (cerr != RdKafka::ERR_NO_ERROR)
                        spdlog::error("Failed to commit offset: {}", RdKafka::err2str(cerr));
                } else {
                    // Failure: leave uncommitted to redeliver — but bound it.
                    const int64_t off = msg->offset();
                    if (off == poison_offset_) {
                        ++poison_count_;
                    } else {
                        poison_offset_ = off;
                        poison_count_ = 1;
                    }
                    if (poison_count_ >= kMaxPoisonAttempts) {
                        spdlog::error(
                            "Kafka poison message {}[{}]@{} failed {}x — committing past it (skipped; "
                            "a production consumer should dead-letter it)",
                            msg->topic_name(),
                            msg->partition(),
                            off,
                            poison_count_);
                        consumer->commitSync(msg.get());
                        poison_offset_ = -1;
                        poison_count_ = 0;
                    }
                }
            }
        }
    }

    void stop_consuming() { consuming = false; }

    bool commit() {
        check_initialized();
        RdKafka::ErrorCode err = consumer->commitSync();
        if (err != RdKafka::ERR_NO_ERROR) {
            spdlog::error("Failed to commit offsets: {}", RdKafka::err2str(err));
            return false;
        }
        return true;
    }

    void shutdown() {
        if (initialized) {
            spdlog::info("Shutting down Kafka consumer");
            stop_consuming();
            if (consumer) {
                consumer->close();
                consumer.reset();
            }
            conf.reset();
            initialized = false;
        }
    }

    bool is_initialized() const { return initialized; }
    bool is_consuming() const { return consuming; }

private:
    void check_initialized() const {
        if (!initialized) {
            throw std::runtime_error("Kafka consumer not initialized");
        }
    }
};

/**
 * @brief Messaging system manager
 */
class MessagingSystem {
private:
    KafkaProducer producer;
    KafkaConsumer consumer;
    bool producer_initialized = false;
    bool consumer_initialized = false;

public:
    void initialize_producer(const std::string& brokers, const std::string& client_id = "cpp_producer") {
        producer.initialize(brokers, client_id);
        producer_initialized = true;
    }

    void initialize_consumer(const std::string& brokers,
                             const std::string& group_id,
                             const std::vector<std::string>& topics,
                             const std::string& auto_offset_reset = "earliest") {
        consumer.initialize(brokers, group_id, topics, auto_offset_reset);
        consumer_initialized = true;
    }

    KafkaProducer& get_producer() {
        if (!producer_initialized)
            throw std::runtime_error("Producer not initialized");
        return producer;
    }

    KafkaConsumer& get_consumer() {
        if (!consumer_initialized)
            throw std::runtime_error("Consumer not initialized");
        return consumer;
    }

    void shutdown() {
        if (consumer_initialized) {
            consumer.shutdown();
            consumer_initialized = false;
        }
        if (producer_initialized) {
            producer.shutdown();
            producer_initialized = false;
        }
    }

    bool has_producer() const { return producer_initialized; }
    bool has_consumer() const { return consumer_initialized; }
};

/**
 * @brief Global messaging instance
 */
inline std::unique_ptr<MessagingSystem> global_messaging = nullptr;

inline void initialize() {
    if (global_messaging != nullptr) {
        throw std::runtime_error("Messaging already initialized");
    }
    global_messaging = std::make_unique<MessagingSystem>();
}

inline MessagingSystem& get() {
    if (global_messaging == nullptr) {
        throw std::runtime_error("Messaging not initialized");
    }
    return *global_messaging;
}

inline bool is_initialized() {
    return global_messaging != nullptr;
}

inline void shutdown() {
    if (global_messaging) {
        global_messaging->shutdown();
        global_messaging.reset();
    }
}

}  // namespace Messaging
