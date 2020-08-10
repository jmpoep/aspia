//
// Aspia Project
// Copyright (C) 2020 Dmitry Chapyshev <dmitry@aspia.ru>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#include "peer/server_authenticator.h"

#include "base/bitset.h"
#include "base/cpuid.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/sys_info.h"
#include "base/crypto/message_decryptor_openssl.h"
#include "base/crypto/message_encryptor_openssl.h"
#include "base/crypto/generic_hash.h"
#include "base/crypto/random.h"
#include "base/crypto/secure_memory.h"
#include "base/crypto/srp_constants.h"
#include "base/crypto/srp_math.h"
#include "base/strings/unicode.h"
#include "build/version.h"
#include "peer/user.h"

namespace peer {

namespace {

constexpr size_t kIvSize = 12;

} // namespace

ServerAuthenticator::ServerAuthenticator(std::shared_ptr<base::TaskRunner> task_runner)
    : Authenticator(std::move(task_runner))
{
    // Nothing
}

ServerAuthenticator::~ServerAuthenticator() = default;

void ServerAuthenticator::setUserList(std::shared_ptr<UserList> user_list)
{
    user_list_ = std::move(user_list);
    DCHECK(user_list_);
}

bool ServerAuthenticator::setPrivateKey(const base::ByteArray& private_key)
{
    // The method must be called before calling start().
    if (state() != State::STOPPED)
        return false;

    if (private_key.empty())
    {
        LOG(LS_ERROR) << "An empty private key is not valid";
        return false;
    }

    key_pair_ = base::KeyPair::fromPrivateKey(private_key);
    if (!key_pair_.isValid())
    {
        LOG(LS_ERROR) << "Failed to load private key. Perhaps the key is incorrect";
        return false;
    }

    encrypt_iv_ = base::Random::byteArray(kIvSize);
    if (encrypt_iv_.empty())
    {
        LOG(LS_ERROR) << "An empty IV is not valid";
        return false;
    }

    return true;
}

bool ServerAuthenticator::setAnonymousAccess(
    AnonymousAccess anonymous_access, uint32_t session_types)
{
    // The method must be called before calling start().
    if (state() != State::STOPPED)
        return false;

    if (anonymous_access == AnonymousAccess::ENABLE)
    {
        if (!key_pair_.isValid())
        {
            LOG(LS_ERROR) << "When anonymous access is enabled, a private key must be installed";
            return false;
        }

        if (!session_types)
        {
            LOG(LS_ERROR) << "When anonymous access is enabled, there must be at least one "
                          << "session for anonymous access";
            return false;
        }

        session_types_ = session_types;
    }
    else
    {
        session_types_ = 0;
    }

    anonymous_access_ = anonymous_access;
    return true;
}

bool ServerAuthenticator::onStarted()
{
    DCHECK(user_list_);

    internal_state_ = InternalState::READ_CLIENT_HELLO;

    // We do not allow anonymous access without a private key.
    if (anonymous_access_ == AnonymousAccess::ENABLE && !key_pair_.isValid())
    {
        finish(FROM_HERE, ErrorCode::UNKNOWN_ERROR);
        return false;
    }

    if (anonymous_access_ == AnonymousAccess::ENABLE)
    {
        // When anonymous access is enabled, a private key must be installed.
        if (!key_pair_.isValid())
        {
            finish(FROM_HERE, ErrorCode::UNKNOWN_ERROR);
            return false;
        }

        // When anonymous access is enabled, there must be at least one session for anonymous access.
        if (!session_types_)
        {
            finish(FROM_HERE, ErrorCode::UNKNOWN_ERROR);
            return false;
        }
    }
    else
    {
        // If anonymous access is disabled, then there should not be allowed sessions by default.
        if (session_types_)
        {
            finish(FROM_HERE, ErrorCode::UNKNOWN_ERROR);
            return false;
        }
    }

    return true;
}

void ServerAuthenticator::onReceived(const base::ByteArray& buffer)
{
    switch (internal_state_)
    {
        case InternalState::READ_CLIENT_HELLO:
            onClientHello(buffer);
            break;

        case InternalState::READ_IDENTIFY:
            onIdentify(buffer);
            break;

        case InternalState::READ_CLIENT_KEY_EXCHANGE:
            onClientKeyExchange(buffer);
            break;

        case InternalState::READ_SESSION_RESPONSE:
            onSessionResponse(buffer);
            break;

        default:
            NOTREACHED();
            break;
    }
}

void ServerAuthenticator::onWritten()
{
    switch (internal_state_)
    {
        case InternalState::SEND_SERVER_HELLO:
        {
            LOG(LS_INFO) << "Sended: ServerHello";

            if (!session_key_.empty())
            {
                if (!onSessionKeyChanged())
                    return;
            }

            switch (identify_)
            {
                case proto::IDENTIFY_SRP:
                {
                    internal_state_ = InternalState::READ_IDENTIFY;
                }
                break;

                case proto::IDENTIFY_ANONYMOUS:
                {
                    internal_state_ = InternalState::SEND_SESSION_CHALLENGE;
                    doSessionChallenge();
                }
                break;

                default:
                    NOTREACHED();
                    break;
            }
        }
        break;

        case InternalState::SEND_SERVER_KEY_EXCHANGE:
        {
            LOG(LS_INFO) << "Sended: ServerKeyExchange";
            internal_state_ = InternalState::READ_CLIENT_KEY_EXCHANGE;
        }
        break;

        case InternalState::SEND_SESSION_CHALLENGE:
        {
            LOG(LS_INFO) << "Sended: SessionChallenge";
            internal_state_ = InternalState::READ_SESSION_RESPONSE;
        }
        break;

        default:
            NOTREACHED();
            break;
    }
}

void ServerAuthenticator::onClientHello(const base::ByteArray& buffer)
{
    LOG(LS_INFO) << "Received: ClientHello";

    proto::ClientHello client_hello;
    if (!base::parse(buffer, &client_hello))
    {
        finish(FROM_HERE, ErrorCode::PROTOCOL_ERROR);
        return;
    }

    LOG(LS_INFO) << "Encryption: " << client_hello.encryption();
    LOG(LS_INFO) << "Identify: " << client_hello.identify();

    if (!(client_hello.encryption() & proto::ENCRYPTION_AES256_GCM) &&
        !(client_hello.encryption() & proto::ENCRYPTION_CHACHA20_POLY1305))
    {
        // No encryption methods supported.
        finish(FROM_HERE, ErrorCode::PROTOCOL_ERROR);
        return;
    }

    identify_ = client_hello.identify();
    switch (identify_)
    {
        // SRP is always supported.
        case proto::IDENTIFY_SRP:
            break;

        case proto::IDENTIFY_ANONYMOUS:
        {
            // If anonymous method is not allowed.
            if (anonymous_access_ != AnonymousAccess::ENABLE)
            {
                finish(FROM_HERE, ErrorCode::ACCESS_DENIED);
                return;
            }
        }
        break;

        default:
        {
            // Unsupported identication method.
            finish(FROM_HERE, ErrorCode::PROTOCOL_ERROR);
            return;
        }
        break;
    }

    proto::ServerHello server_hello;

    if (key_pair_.isValid())
    {
        base::ByteArray peer_public_key = base::fromStdString(client_hello.public_key());
        decrypt_iv_ = base::fromStdString(client_hello.iv());

        if (peer_public_key.empty() != decrypt_iv_.empty())
        {
            finish(FROM_HERE, ErrorCode::PROTOCOL_ERROR);
            return;
        }

        if (!peer_public_key.empty() && !decrypt_iv_.empty())
        {
            base::ByteArray temp = key_pair_.sessionKey(peer_public_key);
            if (temp.empty())
            {
                finish(FROM_HERE, ErrorCode::UNKNOWN_ERROR);
                return;
            }

            session_key_ = base::GenericHash::hash(base::GenericHash::Type::BLAKE2s256, temp);
            if (session_key_.empty())
            {
                finish(FROM_HERE, ErrorCode::UNKNOWN_ERROR);
                return;
            }

            DCHECK(!encrypt_iv_.empty());
            server_hello.set_iv(base::toStdString(encrypt_iv_));
        }
    }

    if ((client_hello.encryption() & proto::ENCRYPTION_AES256_GCM) && base::CPUID::hasAesNi())
    {
        LOG(LS_INFO) << "Both sides have hardware support AES. Using AES256 GCM";
        // If both sides of the connection support AES, then method AES256 GCM is the fastest option.
        server_hello.set_encryption(proto::ENCRYPTION_AES256_GCM);
    }
    else
    {
        LOG(LS_INFO) << "Using ChaCha20 Poly1305";
        // Otherwise, we use ChaCha20+Poly1305. This works faster in the absence of hardware
        // support AES.
        server_hello.set_encryption(proto::ENCRYPTION_CHACHA20_POLY1305);
    }

    // Now we are in the authentication phase.
    internal_state_ = InternalState::SEND_SERVER_HELLO;
    encryption_ = server_hello.encryption();

    LOG(LS_INFO) << "Sending: ServerHello";
    sendMessage(server_hello);
}

void ServerAuthenticator::onIdentify(const base::ByteArray& buffer)
{
    LOG(LS_INFO) << "Received: Identify";

    proto::SrpIdentify identify;
    if (!base::parse(buffer, &identify))
    {
        finish(FROM_HERE, ErrorCode::PROTOCOL_ERROR);
        return;
    }

    LOG(LS_INFO) << "Username:" << identify.username();

    user_name_ = base::utf16FromUtf8(identify.username());
    if (user_name_.empty())
    {
        finish(FROM_HERE, ErrorCode::PROTOCOL_ERROR);
        return;
    }

    do
    {
        const User& user = user_list_->find(user_name_);
        if (user.isValid() && (user.flags & User::ENABLED))
        {
            session_types_ = user.sessions;

            std::optional<base::SrpNgPair> Ng_pair = base::pairByGroup(user.group);
            if (Ng_pair.has_value())
            {
                N_ = base::BigNum::fromStdString(Ng_pair->first);
                g_ = base::BigNum::fromStdString(Ng_pair->second);
                s_ = base::BigNum::fromByteArray(user.salt);
                v_ = base::BigNum::fromByteArray(user.verifier);
                break;
            }
            else
            {
                LOG(LS_ERROR) << "User '" << user.name << "' has an invalid SRP group";
            }
        }

        session_types_ = 0;

        base::GenericHash hash(base::GenericHash::BLAKE2b512);
        hash.addData(user_list_->seedKey());
        hash.addData(identify.username());

        N_ = base::BigNum::fromStdString(base::kSrpNgPair_8192.first);
        g_ = base::BigNum::fromStdString(base::kSrpNgPair_8192.second);
        s_ = base::BigNum::fromByteArray(hash.result());
        v_ = base::SrpMath::calc_v(user_name_, user_list_->seedKey(), s_, N_, g_);
    }
    while (false);

    b_ = base::BigNum::fromByteArray(base::Random::byteArray(128)); // 1024 bits.
    B_ = base::SrpMath::calc_B(b_, N_, g_, v_);

    if (!N_.isValid() || !g_.isValid() || !s_.isValid() || !B_.isValid())
    {
        finish(FROM_HERE, ErrorCode::PROTOCOL_ERROR);
        return;
    }

    internal_state_ = InternalState::SEND_SERVER_KEY_EXCHANGE;
    encrypt_iv_ = base::Random::byteArray(kIvSize);

    proto::SrpServerKeyExchange server_key_exchange;

    server_key_exchange.set_number(N_.toStdString());
    server_key_exchange.set_generator(g_.toStdString());
    server_key_exchange.set_salt(s_.toStdString());
    server_key_exchange.set_b(B_.toStdString());
    server_key_exchange.set_iv(base::toStdString(encrypt_iv_));

    LOG(LS_INFO) << "Sending: ServerKeyExchange";
    sendMessage(server_key_exchange);
}

void ServerAuthenticator::onClientKeyExchange(const base::ByteArray& buffer)
{
    LOG(LS_INFO) << "Received: ClientKeyExchange";

    proto::SrpClientKeyExchange client_key_exchange;
    if (!base::parse(buffer, &client_key_exchange))
    {
        finish(FROM_HERE, ErrorCode::PROTOCOL_ERROR);
        return;
    }

    A_ = base::BigNum::fromStdString(client_key_exchange.a());
    decrypt_iv_ = base::fromStdString(client_key_exchange.iv());

    if (!A_.isValid() || decrypt_iv_.empty())
    {
        finish(FROM_HERE, ErrorCode::PROTOCOL_ERROR);
        return;
    }

    base::ByteArray srp_key = createSrpKey();
    if (srp_key.empty())
    {
        finish(FROM_HERE, ErrorCode::UNKNOWN_ERROR);
        return;
    }

    switch (encryption_)
    {
        // AES256-GCM and ChaCha20-Poly1305 requires 256 bit key.
        case proto::ENCRYPTION_AES256_GCM:
        case proto::ENCRYPTION_CHACHA20_POLY1305:
        {
            base::GenericHash hash(base::GenericHash::BLAKE2s256);

            if (!session_key_.empty())
                hash.addData(session_key_);
            hash.addData(srp_key);

            session_key_ = hash.result();
        }
        break;

        default:
        {
            finish(FROM_HERE, ErrorCode::UNKNOWN_ERROR);
            return;
        }
        break;
    }

    if (!onSessionKeyChanged())
        return;

    internal_state_ = InternalState::SEND_SESSION_CHALLENGE;
    doSessionChallenge();
}

void ServerAuthenticator::doSessionChallenge()
{
    proto::SessionChallenge session_challenge;
    session_challenge.set_session_types(session_types_);

    proto::Version* version = session_challenge.mutable_version();
    version->set_major(ASPIA_VERSION_MAJOR);
    version->set_minor(ASPIA_VERSION_MINOR);
    version->set_patch(ASPIA_VERSION_PATCH);

#if defined(OS_WIN)
    session_challenge.set_os_type(proto::OS_TYPE_WINDOWS);
#else
#error Not implemented
#endif

    session_challenge.set_computer_name(base::SysInfo::computerName());
    session_challenge.set_cpu_cores(base::SysInfo::processorCores());

    LOG(LS_INFO) << "Sending: SessionChallenge";
    sendMessage(session_challenge);
}

void ServerAuthenticator::onSessionResponse(const base::ByteArray& buffer)
{
    LOG(LS_INFO) << "Received: SessionResponse";

    proto::SessionResponse session_response;
    if (!base::parse(buffer, &session_response))
    {
        finish(FROM_HERE, ErrorCode::PROTOCOL_ERROR);
        return;
    }

    setPeerVersion(session_response.version());

    LOG(LS_INFO) << "Client Session Type: " << session_response.session_type();
    LOG(LS_INFO) << "Client Version: " << peerVersion();
    LOG(LS_INFO) << "Client Name: " << session_response.computer_name();
    LOG(LS_INFO) << "Client OS: " << osTypeToString(session_response.os_type());
    LOG(LS_INFO) << "Client CPU Cores: " << session_response.cpu_cores();

    base::BitSet<uint32_t> session_type = session_response.session_type();
    if (session_type.count() != 1)
    {
        finish(FROM_HERE, ErrorCode::PROTOCOL_ERROR);
        return;
    }

    session_type_ = session_type.value();
    if (!(session_types_ & session_type_))
    {
        finish(FROM_HERE, ErrorCode::SESSION_DENIED);
        return;
    }

    // Authentication completed successfully.
    finish(FROM_HERE, ErrorCode::SUCCESS);
}

base::ByteArray ServerAuthenticator::createSrpKey()
{
    if (!base::SrpMath::verify_A_mod_N(A_, N_))
    {
        LOG(LS_ERROR) << "SrpMath::verify_A_mod_N failed";
        return base::ByteArray();
    }

    base::BigNum u = base::SrpMath::calc_u(A_, B_, N_);
    base::BigNum server_key = base::SrpMath::calcServerKey(A_, v_, u, b_, N_);

    return server_key.toByteArray();
}

} // namespace peer