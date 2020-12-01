#include "utils/Settings.h"
#include "shims/TorControl.h"
#include "shims/TorManager.h"
#include "shims/UserIdentity.h"
#include "shims/ConversationModel.h"
namespace
{
    constexpr int consumeInterval = 10;

    // data
    std::unique_ptr<QTimer> taskTimer;
    std::vector<std::function<void()>> taskQueue;
    std::mutex taskQueueLock;

    void consume_tasks()
    {
        // get sole access to the task queue
        static decltype(taskQueue) localTaskQueue;
        {
            std::lock_guard<std::mutex> lock(taskQueueLock);
            std::swap(taskQueue, localTaskQueue);
        }

        // consume all of our tasks
        for(auto task : localTaskQueue)
        {
            try
            {
                task();
            }
            catch(std::exception& ex)
            {
                qDebug() << "Exception thrown from task: " << ex.what();
            }
        }

        // clear out our queue
        localTaskQueue.clear();

		// schedule us to run again
	    QTimer::singleShot(consumeInterval, &consume_tasks);
    }

    template<typename FUNC>
    void push_task(FUNC&& func)
    {
        // acquire lock on the queue and push our received functor
        std::lock_guard<std::mutex> lock(taskQueueLock);
        taskQueue.push_back(std::move(func));
    }

    // converts the our tego_user_id_t to ricochet's contactId in the form ricochet:serviceidserviceidserviceid...
    QString tegoUserIdToContactId(const tego_user_id_t* user)
    {
        std::unique_ptr<tego_v3_onion_service_id> serviceId;
        tego_user_id_get_v3_onion_service_id(user, tego::out(serviceId), tego::throw_on_error());

        char serviceIdRaw[TEGO_V3_ONION_SERVICE_ID_SIZE] = {0};
        tego_v3_onion_service_id_to_string(serviceId.get(), serviceIdRaw, sizeof(serviceIdRaw), tego::throw_on_error());

        QString contactId = QString("ricochet:") + QString::fromUtf8(serviceIdRaw, TEGO_V3_ONION_SERVICE_ID_LENGTH);

        return contactId;
    }

    shims::ContactUser* contactUserFromContactId(const QString& contactId)
    {
        auto userIdentity = shims::UserIdentity::userIdentity;
        auto contactsManager = userIdentity->getContacts();

        auto contactUser = contactsManager->getShimContactByContactId(contactId);
        return contactUser;
    }

    //
    // libtego callbacks
    //

    void on_tor_error_occurred(
        tego_context_t*,
        tego_tor_error_origin_t origin,
        const tego_error_t* error)
    {
        // route the error message to the appropriate component
        QString errorMsg = tego_error_get_message(error);
        logger::println("tor error : {}", errorMsg);
        push_task([=]() -> void
        {
            switch(origin)
            {
                case tego_tor_error_origin_control:
                {
                    shims::TorControl::torControl->setErrorMessage(errorMsg);
                }
                break;
                case tego_tor_error_origin_manager:
                {
                    shims::TorManager::torManager->setErrorMessage(errorMsg);
                }
                break;
            }
        });
    }

    void on_update_tor_daemon_config_succeeded(
        tego_context_t*,
        tego_bool_t success)
    {
        push_task([=]() -> void
        {
            logger::println("tor daemon config succeeded : {}", success);
            auto torControl = shims::TorControl::torControl;
            if (torControl->m_setConfigurationCommand != nullptr)
            {
                torControl->m_setConfigurationCommand->onFinished(success);
                torControl->m_setConfigurationCommand = nullptr;
            }
        });
    }

    void on_tor_control_status_changed(
        tego_context_t*,
        tego_tor_control_status_t status)
    {
        push_task([=]() -> void
        {
            logger::println("new control status : {}", status);
            shims::TorControl::torControl->setStatus(static_cast<shims::TorControl::Status>(status));
        });
    }

    void on_tor_process_status_changed(
        tego_context_t*,
        tego_tor_process_status_t status)
    {
        push_task([=]() -> void
        {
            logger::println("new process status : {}", status);
            auto torManger = shims::TorManager::torManager;
            switch(status)
            {
                case tego_tor_process_status_running:
                    torManger->setRunning("Yes");
                    break;
                case tego_tor_process_status_external:
                    torManger->setRunning("External");
                    break;
                default:
                    torManger->setRunning("No");
                    break;
            }
        });
    }

    void on_tor_network_status_changed(
        tego_context_t*,
        tego_tor_network_status_t status)
    {
        push_task([=]() -> void
        {
            logger::println("new network status : {}", status);
            auto torControl = shims::TorControl::torControl;
            switch(status)
            {
                case tego_tor_network_status_unknown:
                    torControl->setTorStatus(shims::TorControl::TorUnknown);
                    break;
                case tego_tor_network_status_ready:
                    torControl->setTorStatus(shims::TorControl::TorReady);
                    break;
                case tego_tor_network_status_offline:
                    torControl->setTorStatus(shims::TorControl::TorOffline);
                    break;
            }
        });
    }

    void on_tor_bootstrap_status_changed(
        tego_context_t*,
        int32_t progress,
        tego_tor_bootstrap_tag_t tag)
    {
        push_task([=]() -> void
        {
            logger::println("bootstrap status : {{ progress : {}, tag : {} }}", progress, (int)tag);
            auto torControl = shims::TorControl::torControl;
            emit torControl->bootstrapStatusChanged();
        });
    }

    void on_tor_log_received(
        tego_context_t*,
        const char* message,
        size_t messageLength)
    {
        auto messageString = QString::fromUtf8(message, messageLength);
        push_task([=]()-> void
        {
            auto torManager = shims::TorManager::torManager;
            emit torManager->logMessage(messageString);
        });
    }

    void on_host_user_state_changed(
        tego_context_t*,
        tego_host_user_state_t state)
    {
        logger::println("new host user state : {}", state);
        push_task([=]() -> void
        {
            auto userIdentity = shims::UserIdentity::userIdentity;
            switch(state)
            {
                case tego_host_user_state_offline:
                    userIdentity->setOnline(false);
                    break;
                case tego_host_user_state_online:
                    userIdentity->setOnline(true);
                    break;
                default:
                    break;
            }
        });
    }

    void on_chat_request_response_received(
        tego_context_t*,
        const tego_user_id_t* userId,
        tego_bool_t requestAccepted)
    {
        std::unique_ptr<tego_v3_onion_service_id> serviceId;
        tego_user_id_get_v3_onion_service_id(userId, tego::out(serviceId), tego::throw_on_error());

        char serviceIdRaw[TEGO_V3_ONION_SERVICE_ID_SIZE] = {0};
        tego_v3_onion_service_id_to_string(serviceId.get(), serviceIdRaw, sizeof(serviceIdRaw), tego::throw_on_error());

        QString serviceIdString(serviceIdRaw);
        push_task([=]() -> void
        {
            logger::trace();
            if (requestAccepted) {
                // delete the request block entirely like in OutgoingContactRequest::removeRequest
                SettingsObject so(QStringLiteral("contacts.%1").arg(serviceIdString));
                so.unset("request");
            }
        });
    }

    void on_user_status_changed(
        tego_context_t*,
        const tego_user_id_t* userId,
        tego_user_status_t status)
    {
        logger::trace();

        std::unique_ptr<tego_v3_onion_service_id> serviceId;
        tego_user_id_get_v3_onion_service_id(userId, tego::out(serviceId), tego::throw_on_error());

        char serviceIdRaw[TEGO_V3_ONION_SERVICE_ID_SIZE] = {0};
        tego_v3_onion_service_id_to_string(serviceId.get(), serviceIdRaw, sizeof(serviceIdRaw), tego::throw_on_error());

        logger::println("user status changed -> service id : {}, status : {}", serviceIdRaw, (int)status);

        QString serviceIdString(serviceIdRaw);
        push_task([=]() -> void
        {
            constexpr auto ContactUser_RequestPending = 2;
            if (status == ContactUser_RequestPending)
            {
                SettingsObject so(QStringLiteral("contacts.%1").arg(serviceIdString));
                so.write("request.status", 1);
            }
        });
    }

    void on_message_received(
        tego_context_t*,
        const tego_user_id_t* sender,
        tego_time_t timestamp,
        tego_message_id_t messageId,
        const char* message,
        size_t messageLength)
    {
        auto contactId = tegoUserIdToContactId(sender);
        auto messageString = QString::fromUtf8(message, messageLength);

        push_task([=]() -> void
        {
            auto contactUser = contactUserFromContactId(contactId);
            Q_ASSERT(contactUser != nullptr);
            auto conversationModel = contactUser->conversation();
            Q_ASSERT(conversationModel != nullptr);

            conversationModel->messageReceived(messageId, QDateTime::fromMSecsSinceEpoch(timestamp), messageString);
        });
    }

    void on_message_acknowledged(
        tego_context_t*,
        const tego_user_id_t* userId,
        tego_message_id_t messageId,
        tego_bool_t messageAccepted)
    {
        logger::trace();
        logger::println(" userId : {}", (void*)userId);
        logger::println(" messageId : {}", messageId);
        logger::println(" messageAccepted : {}", messageAccepted);

        QString contactId = tegoUserIdToContactId(userId);
        push_task([=]() -> void
        {
            logger::trace();
            auto contactsManager = shims::UserIdentity::userIdentity->getContacts();
            auto contactUser = contactsManager->getShimContactByContactId(contactId);
            auto conversationModel = contactUser->conversation();
            conversationModel->messageAcknowledged(messageId, static_cast<bool>(messageAccepted));
        });
    }

    void on_new_identity_created(
        tego_context_t*,
        const tego_ed25519_private_key_t* privateKey)
    {
        // convert privateKey to KeyBlob
        char rawKeyBlob[TEGO_ED25519_KEYBLOB_SIZE] = {0};
        tego_ed25519_keyblob_from_ed25519_private_key(
            rawKeyBlob,
            sizeof(rawKeyBlob),
            privateKey,
            tego::throw_on_error());

        QString keyBlob(rawKeyBlob);

        push_task([=]() -> void
        {
            SettingsObject so(QStringLiteral("identity"));
            so.write("serviceKey", keyBlob);
        });
    }
}

void init_libtego_callbacks(tego_context_t* context)
{
    // start triggering our consume queue
    QTimer::singleShot(consumeInterval, &consume_tasks);

    //
    // register each of our callbacks with libtego
    //

    tego_context_set_tor_error_occurred_callback(
        context,
        &on_tor_error_occurred,
        tego::throw_on_error());

    tego_context_set_update_tor_daemon_config_succeeded_callback(
        context,
        &on_update_tor_daemon_config_succeeded,
        tego::throw_on_error());

    tego_context_set_tor_control_status_changed_callback(
        context,
        &on_tor_control_status_changed,
        tego::throw_on_error());

    tego_context_set_tor_process_status_changed_callback(
        context,
        &on_tor_process_status_changed,
        tego::throw_on_error());

    tego_context_set_tor_network_status_changed_callback(
        context,
        &on_tor_network_status_changed,
        tego::throw_on_error());

    tego_context_set_tor_bootstrap_status_changed_callback(
        context,
        &on_tor_bootstrap_status_changed,
        tego::throw_on_error());

    tego_context_set_tor_log_received_callback(
        context,
        &on_tor_log_received,
        tego::throw_on_error());

    tego_context_set_host_user_state_changed_callback(
        context,
        &on_host_user_state_changed,
        tego::throw_on_error());

    tego_context_set_chat_request_response_received_callback(
        context,
        &on_chat_request_response_received,
        tego::throw_on_error());

    tego_context_set_user_status_changed_callback(
        context,
        &on_user_status_changed,
        tego::throw_on_error());

    tego_context_set_message_received_callback(
        context,
        &on_message_received,
        tego::throw_on_error());

    tego_context_set_message_acknowledged_callback(
        context,
        &on_message_acknowledged,
        tego::throw_on_error());

    tego_context_set_new_identity_created_callback(
        context,
        &on_new_identity_created,
        tego::throw_on_error());
}