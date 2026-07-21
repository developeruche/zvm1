// evmone: Fast Ethereum Virtual Machine implementation
// Copyright 2022 The evmone Authors.
// SPDX-License-Identifier: Apache-2.0

#include "host.hpp"
#include "precompiles.hpp"
#include <evmone/constants.hpp>
#include <evmone/vm.hpp>

namespace evmone::state
{
bool Host::account_exists(const address& addr) const noexcept
{
    const auto* const acc = m_state.find(addr);
    return acc != nullptr && (m_rev < EVMC_SPURIOUS_DRAGON || !acc->is_empty());
}

bytes32 Host::get_storage(const address& addr, const bytes32& key) const noexcept
{
    return m_state.get_storage(addr, key).current;
}

evmc_storage_status Host::set_storage(
    const address& addr, const bytes32& key, const bytes32& value) noexcept
{
    // Follow EVMC documentation https://evmc.ethereum.org/storagestatus.html#autotoc_md3
    // and EIP-2200 specification https://eips.ethereum.org/EIPS/eip-2200.

    auto& storage_slot = m_state.get_storage(addr, key);
    const auto& [current, original, _] = storage_slot;

    const auto dirty = original != current;
    const auto restored = original == value;
    const auto current_is_zero = is_zero(current);
    const auto value_is_zero = is_zero(value);

    auto status = EVMC_STORAGE_ASSIGNED;  // All other cases.
    if (!dirty && !restored)
    {
        if (current_is_zero)
            status = EVMC_STORAGE_ADDED;  // 0 → 0 → Z
        else if (value_is_zero)
            status = EVMC_STORAGE_DELETED;  // X → X → 0
        else
            status = EVMC_STORAGE_MODIFIED;  // X → X → Z
    }
    else if (dirty && !restored)
    {
        if (current_is_zero && !value_is_zero)
            status = EVMC_STORAGE_DELETED_ADDED;  // X → 0 → Z
        else if (!current_is_zero && value_is_zero)
            status = EVMC_STORAGE_MODIFIED_DELETED;  // X → Y → 0
    }
    else if (dirty)
    {
        assert(restored);  // Always true.
        if (current_is_zero)
            status = EVMC_STORAGE_DELETED_RESTORED;  // X → 0 → X
        else if (value_is_zero)
            status = EVMC_STORAGE_ADDED_DELETED;  // 0 → Y → 0
        else
            status = EVMC_STORAGE_MODIFIED_RESTORED;  // X → Y → X
    }

    // Amsterdam (EIP-8037): STORAGE_SET state gas on slot creation,
    // credited back when the created slot is cleared again.
    if (m_rev >= EVMC_AMSTERDAM)
    {
        if (status == EVMC_STORAGE_ADDED)
            m_state_gas_used += 97920;
        else if (status == EVMC_STORAGE_ADDED_DELETED)
            m_state_gas_used -= 97920;
    }

    // In Berlin this is handled in access_storage().
    if (m_rev < EVMC_BERLIN)
        m_state.journal_storage_change(addr, key, storage_slot);
    storage_slot.current = value;  // Update current value.
    return status;
}

uint256be Host::get_balance(const address& addr) const noexcept
{
    const auto* const acc = m_state.find(addr);
    return (acc != nullptr) ? intx::be::store<uint256be>(acc->balance) : uint256be{};
}

namespace
{
/// Check if an existing account is the "create collision"
/// as defined in the [EIP-7610](https://eips.ethereum.org/EIPS/eip-7610).
[[nodiscard]] bool is_create_collision(const Account& acc) noexcept
{
    // TODO: This requires much more testing:
    // - what if an account had storage but is destructed?
    // - what if an account had cold storage but it was emptied?
    // - what if an account without cold storage gain one?
    if (acc.nonce != 0)
        return true;
    if (acc.code_hash != Account::EMPTY_CODE_HASH)
        return true;
    if (acc.has_initial_storage)
        return true;

    // The hot storage is ignored because it can contain elements from access list.
    // TODO: Is this correct for destructed accounts?
    //assert(!acc.destructed && "untested");
    return false;
}
}  // namespace

size_t Host::get_code_size(const address& addr) const noexcept
{
    const auto raw_code = m_state.get_code(addr);
    return raw_code.size();
}

bytes32 Host::get_code_hash(const address& addr) const noexcept
{
    const auto* const acc = m_state.find(addr);
    if (acc == nullptr || acc->is_empty())
        return {};

    return acc->code_hash;
}

size_t Host::copy_code(const address& addr, size_t code_offset, uint8_t* buffer_data,
    size_t buffer_size) const noexcept
{
    const auto code = m_state.get_code(addr);
    const auto code_slice = code.substr(std::min(code_offset, code.size()));
    const auto num_bytes = std::min(buffer_size, code_slice.size());
    std::copy_n(code_slice.begin(), num_bytes, buffer_data);
    return num_bytes;
}

bool Host::selfdestruct(const address& addr, const address& beneficiary) noexcept
{
    if (m_state.find(beneficiary) == nullptr)
        m_state.journal_create(beneficiary, false);
    auto& acc = m_state.get(addr);
    const auto balance = acc.balance;

    // Amsterdam (EIP-8037): NEW_ACCOUNT state gas for the beneficiary sweep
    // (the regular ACCOUNT_WRITE part is charged by the instruction).
    if (m_rev >= EVMC_AMSTERDAM && balance != 0 && !account_exists(beneficiary))
        m_state_gas_used += 183600;

    auto& beneficiary_acc = m_state.touch(beneficiary);

    m_state.journal_balance_change(beneficiary, beneficiary_acc.balance);
    m_state.journal_balance_change(addr, balance);

    // Amsterdam (EIP-7708): the sweep emits a transfer log.
    if (m_rev >= EVMC_AMSTERDAM && addr != beneficiary)
        emit_transfer_log(addr, beneficiary, balance);

    if (m_rev >= EVMC_CANCUN && !acc.just_created)
    {
        // EIP-6780:
        // "SELFDESTRUCT is executed in a transaction that is not the same
        // as the contract invoking SELFDESTRUCT was created"
        acc.balance = 0;
        beneficiary_acc.balance += balance;  // Keep balance if acc is the beneficiary.

        // Return "selfdestruct not registered".
        // In practice this affects only refunds before Cancun.
        return false;
    }

    // Transfer may happen multiple times per single account as account's balance
    // can be increased with a call following previous selfdestruct.
    // Amsterdam (EIP-8246): a self-beneficiary keeps its balance (no burn).
    if (m_rev >= EVMC_AMSTERDAM && addr == beneficiary)
    {
        // Balance preserved; deletion below keeps it via build_diff.
    }
    else
    {
        beneficiary_acc.balance += balance;
        acc.balance = 0;  // Zero balance if acc is the beneficiary (burn, pre-Amsterdam).
    }

    // Mark the destruction if not done already.
    if (!acc.destructed)
    {
        m_state.journal_destruct(addr);
        acc.destructed = true;
        return true;
    }
    return false;
}

address compute_create_address(const address& sender, uint64_t sender_nonce) noexcept
{
    static constexpr auto RLP_STR_BASE = 0x80;
    static constexpr auto RLP_LIST_BASE = 0xc0;
    static constexpr auto ADDRESS_SIZE = sizeof(sender);
    static constexpr std::ptrdiff_t MAX_NONCE_SIZE = sizeof(sender_nonce);

    uint8_t buffer[ADDRESS_SIZE + MAX_NONCE_SIZE + 3];  // 3 for RLP prefix bytes.
    auto p = &buffer[1];                                // Skip RLP list prefix for now.
    *p++ = RLP_STR_BASE + ADDRESS_SIZE;                 // Set RLP string prefix for address.
    p = std::copy_n(sender.bytes, ADDRESS_SIZE, p);

    if (sender_nonce < RLP_STR_BASE)  // Short integer encoding including 0 as empty string (0x80).
    {
        *p++ = sender_nonce != 0 ? static_cast<uint8_t>(sender_nonce) : RLP_STR_BASE;
    }
    else  // Prefixed integer encoding.
    {
        // TODO: bit_width returns int after [LWG 3656](https://cplusplus.github.io/LWG/issue3656).
        // NOLINTNEXTLINE(readability-redundant-casting)
        const auto num_nonzero_bytes = static_cast<int>((std::bit_width(sender_nonce) + 7) / 8);
        *p++ = static_cast<uint8_t>(RLP_STR_BASE + num_nonzero_bytes);
        intx::be::unsafe::store(p, sender_nonce);
        p = std::shift_left(p, p + MAX_NONCE_SIZE, MAX_NONCE_SIZE - num_nonzero_bytes);
    }

    const auto total_size = static_cast<size_t>(p - buffer);
    buffer[0] = static_cast<uint8_t>(RLP_LIST_BASE + (total_size - 1));  // Set the RLP list prefix.

    const auto base_hash = keccak256({buffer, total_size});
    address addr;
    std::copy_n(&base_hash.bytes[sizeof(base_hash) - ADDRESS_SIZE], ADDRESS_SIZE, addr.bytes);
    return addr;
}

address compute_create2_address(
    const address& sender, const bytes32& salt, bytes_view init_code) noexcept
{
    const auto init_code_hash = keccak256(init_code);
    uint8_t buffer[1 + sizeof(sender) + sizeof(salt) + sizeof(init_code_hash)];
    // static_assert(std::size(buffer) == 85);
    auto it = std::begin(buffer);
    *it++ = 0xff;
    it = std::copy_n(sender.bytes, sizeof(sender), it);
    it = std::copy_n(salt.bytes, sizeof(salt), it);
    std::copy_n(init_code_hash.bytes, sizeof(init_code_hash), it);
    const auto base_hash = keccak256({buffer, std::size(buffer)});
    address addr;
    std::copy_n(&base_hash.bytes[sizeof(base_hash) - sizeof(addr)], sizeof(addr), addr.bytes);
    return addr;
}

std::optional<evmc_message> Host::prepare_message(evmc_message msg) noexcept
{
    assert(msg.kind != EVMC_EOFCREATE);
    if (msg.depth == 0 || msg.kind == EVMC_CREATE || msg.kind == EVMC_CREATE2)
    {
        auto& sender_acc = m_state.get(msg.sender);

        // EIP-2681 (already checked for depth 0 during transaction validation).
        if (sender_acc.nonce == Account::NonceMax)
            return {};  // Light early exception.

        if (msg.depth != 0)
        {
            m_state.journal_bump_nonce(msg.sender);
            ++sender_acc.nonce;  // Bump sender nonce.
        }

        if (msg.kind == EVMC_CREATE || msg.kind == EVMC_CREATE2)
        {
            // Compute and set the address of the account being created.
            //assert(msg.recipient == address{});
            //assert(msg.code_address == address{});
            // Nonce was already incremented, but creation calculation needs non-incremented value
            //assert(sender_acc.nonce != 0);
            const auto creation_sender_nonce = sender_acc.nonce - 1;
            if (msg.kind == EVMC_CREATE)
                msg.recipient = compute_create_address(msg.sender, creation_sender_nonce);
            else
            {
                assert(msg.kind == EVMC_CREATE2);
                msg.recipient = compute_create2_address(
                    msg.sender, msg.create2_salt, {msg.input_data, msg.input_size});
            }

            // By EIP-2929, the access to new created address is never reverted.
            access_account(msg.recipient);
        }
    }

    return msg;
}

evmc::Result Host::create(const evmc_message& msg) noexcept
{
    assert(msg.kind == EVMC_CREATE || msg.kind == EVMC_CREATE2);

    auto* new_acc = m_state.find(msg.recipient);
    const bool new_acc_exists = new_acc != nullptr;
    // Amsterdam (EIP-8037): NEW_ACCOUNT state gas for a creation target
    // whose account leaf is not alive. Charged from the create frame's gas
    // (EELS charges the parent pre-split; the 1/64 retention difference is
    // a known Stage-1 approximation).
    int64_t create_state_gas = 0;
    if (m_rev >= EVMC_AMSTERDAM &&
        (!new_acc_exists || (new_acc->nonce == 0 && new_acc->balance == 0 &&
                                new_acc->code_hash == Account::EMPTY_CODE_HASH)))
        create_state_gas = 183600;
    if (!new_acc_exists)
        new_acc = &m_state.insert(msg.recipient);
    else if (is_create_collision(*new_acc))
        return evmc::Result{EVMC_FAILURE};  // TODO: Add EVMC errors for creation failures.
    m_state.journal_create(msg.recipient, new_acc_exists);

    //assert(new_acc != nullptr);
    //assert(new_acc->nonce == 0);

    if (m_rev >= EVMC_SPURIOUS_DRAGON)
        new_acc->nonce = 1;  // No need to journal: create revert will 0 the nonce.

    new_acc->just_created = true;

    auto& sender_acc = m_state.get(msg.sender);  // TODO: Duplicated account lookup.
    const auto value = intx::be::load<intx::uint256>(msg.value);
    //assert(sender_acc.balance >= value && "EVM must guarantee balance");
    m_state.journal_balance_change(msg.sender, sender_acc.balance);
    m_state.journal_balance_change(msg.recipient, new_acc->balance);
    sender_acc.balance -= value;
    new_acc->balance += value;  // The new account may be prefunded.

    // Amsterdam (EIP-7708): value endowment emits a transfer log.
    if (m_rev >= EVMC_AMSTERDAM && address{msg.sender} != address{msg.recipient})
        emit_transfer_log(msg.sender, msg.recipient, value);

    auto create_msg = msg;
    create_msg.input_data = nullptr;
    create_msg.input_size = 0;
    if (create_state_gas != 0)
    {
        if (create_msg.gas < create_state_gas)
            return evmc::Result{EVMC_OUT_OF_GAS};
        create_msg.gas -= create_state_gas;
        m_state_gas_used += create_state_gas;
    }
    const bytes_view initcode{msg.input_data, msg.input_size};
    auto result = m_vm.execute(*this, m_rev, create_msg, initcode.data(), initcode.size());
    if (result.status_code != EVMC_SUCCESS)
    {
        // The failed frame's state rolls back, so its NEW_ACCOUNT state gas
        // rolls back with it — but only a REVERT returns the gas (EELS
        // refill_frame_state_gas: an exceptional halt consumes gas_left
        // after the refill, so the charge is burned with the frame).
        if (create_state_gas != 0)
        {
            if (result.status_code == EVMC_REVERT)
                result.gas_left += create_state_gas;
            m_state_gas_used -= create_state_gas;
        }
        result.create_address = msg.recipient;
        return result;
    }

    auto gas_left = result.gas_left;
    //assert(gas_left >= 0);

    const bytes_view code{result.output_data, result.output_size};

    // Amsterdam (EIP-7954): max contract size 0x10000.
    const size_t max_code_size = m_rev >= EVMC_AMSTERDAM ? 0x10000 : MAX_CODE_SIZE;
    if (m_rev >= EVMC_SPURIOUS_DRAGON && code.size() > max_code_size)
        return evmc::Result{EVMC_FAILURE};

    // Code deployment cost. Amsterdam (EIP-2780/8037): keccak hashing cost
    // (6/word, regular) plus per-byte state gas (1530/byte) instead of the
    // flat 200/byte.
    int64_t cost = 0;
    if (m_rev >= EVMC_AMSTERDAM)
    {
        const auto num_words = (std::ssize(code) + 31) / 32;
        const auto deposit_state_gas = std::ssize(code) * 1530;
        cost = num_words * 6 + deposit_state_gas;
        if (gas_left >= cost)
            m_state_gas_used += deposit_state_gas;
    }
    else
        cost = std::ssize(code) * 200;
    gas_left -= cost;
    if (gas_left < 0)
    {
        return (m_rev == EVMC_FRONTIER) ?
                   evmc::Result{EVMC_SUCCESS, result.gas_left, result.gas_refund, msg.recipient} :
                   evmc::Result{EVMC_FAILURE};
    }

    if (!code.empty())
    {
        // EIP-3541: Reject new contract code starting with the 0xEF byte.
        if (m_rev >= EVMC_LONDON && code[0] == 0xEF)
            return evmc::Result{EVMC_CONTRACT_VALIDATION_FAILURE};

        new_acc->code_hash = keccak256(code);
        new_acc->code = code;
        new_acc->code_changed = true;
    }

    return evmc::Result{result.status_code, gas_left, result.gas_refund, msg.recipient};
}

evmc::Result Host::execute_message(const evmc_message& msg) noexcept
{
    assert(msg.kind != EVMC_EOFCREATE);
    if (msg.kind == EVMC_CREATE || msg.kind == EVMC_CREATE2)
        return create(msg);

    if (msg.kind == EVMC_CALL)
    {
        const auto exists = m_state.find(msg.recipient) != nullptr;
        if (!exists)
            m_state.journal_create(msg.recipient, exists);
    }

    if (msg.kind == EVMC_CALL)
    {
        if (evmc::is_zero(msg.value))
            m_state.touch(msg.recipient);
        else
        {
            // Amsterdam (EIP-8037): tally NEW_ACCOUNT state gas for a value
            // transfer materializing a dead account. The gas deduction
            // happens at the CALL instruction (inner frames) or in
            // transition() (the top frame).
            if (m_rev >= EVMC_AMSTERDAM && !account_exists(msg.recipient))
                m_state_gas_used += 183600;

            // We skip touching if we send value, because account cannot end up empty.
            // It will either have value, or code that transfers this value out, or will be
            // selfdestructed anyway.
            auto& dst_acc = m_state.get_or_insert(msg.recipient);

            // Transfer value: sender → recipient.
            // The sender's balance is already checked therefore the sender account must exist.
            const auto value = intx::be::load<intx::uint256>(msg.value);
            assert(m_state.get(msg.sender).balance >= value);
            m_state.journal_balance_change(msg.sender, m_state.get(msg.sender).balance);
            m_state.journal_balance_change(msg.recipient, dst_acc.balance);
            m_state.get(msg.sender).balance -= value;
            dst_acc.balance += value;

            // Amsterdam (EIP-7708): emit the transfer log.
            if (m_rev >= EVMC_AMSTERDAM && address{msg.sender} != address{msg.recipient})
                emit_transfer_log(msg.sender, msg.recipient, value);
        }
    }

    // Calls to precompile address via EIP-7702 delegation execute empty code instead of precompile.
    if ((msg.flags & EVMC_DELEGATED) == 0 && is_precompile(m_rev, msg.code_address))
        return call_precompile(m_rev, msg);

    const auto code_acc = m_state.find(msg.code_address);
    if (code_acc == nullptr || code_acc->code_hash == Account::EMPTY_CODE_HASH)
        return evmc::Result{EVMC_SUCCESS, msg.gas};

    auto* my_vm = static_cast<VM*>(m_vm.get_raw_pointer());
    auto opt_result = my_vm->execute_cached_code(*this, m_rev, msg, code_acc->code_hash,
        [this](const address& addr) { return m_state.get_code(addr); });
    if (opt_result.has_value())
        return std::move(*opt_result);

    // TODO: get_code() performs the account lookup. Add a way to get an account with code?
    const auto code = m_state.get_code(msg.code_address);
    if (code.empty())
        return evmc::Result{EVMC_SUCCESS, msg.gas};  // Skip trivial execution.

    return m_vm.execute(*this, m_rev, msg, code.data(), code.size());
}

evmc::Result Host::call(const evmc_message& orig_msg) noexcept
{
    const auto msg = prepare_message(orig_msg);
    if (!msg.has_value())
        return evmc::Result{EVMC_FAILURE, orig_msg.gas};  // Light exception.

    const auto logs_checkpoint = m_logs.size();
    const auto state_checkpoint = m_state.checkpoint();
    const auto state_gas_checkpoint = m_state_gas_used;

    auto result = execute_message(*msg);

    if (result.status_code != EVMC_SUCCESS)
    {
        static constexpr auto addr_03 = 0x03_address;
        auto* const acc_03 = m_state.find(addr_03);
        const auto is_03_touched = acc_03 != nullptr && acc_03->erase_if_empty;

        // Amsterdam: the frame's state changes roll back, so the state gas
        // its subtree consumed rolls back with them (EELS
        // refill_frame_state_gas). A REVERT returns the refilled gas to the
        // parent with gas_left; an exceptional halt consumes it anyway.
        if (m_rev >= EVMC_AMSTERDAM)
        {
            const auto state_gas_delta = m_state_gas_used - state_gas_checkpoint;
            m_state_gas_used = state_gas_checkpoint;
            if (result.status_code == EVMC_REVERT && state_gas_delta > 0)
                result.gas_left += state_gas_delta;
        }

        // Revert.
        m_state.rollback(state_checkpoint);
        m_logs.resize(logs_checkpoint);

        // The 0x03 quirk: the touch on this address is never reverted.
        if (is_03_touched && m_rev >= EVMC_SPURIOUS_DRAGON)
            m_state.touch(addr_03);
    }
    return result;
}

evmc_tx_context Host::get_tx_context() const noexcept
{
    // TODO: The effective gas price is already computed in transaction validation.
    // TODO: The effective gas price calculation is broken for system calls (gas price 0).
    assert(m_tx.max_gas_price >= m_block.base_fee || m_tx.max_gas_price == 0);
    const auto priority_gas_price =
        std::min(m_tx.max_priority_gas_price, m_tx.max_gas_price - m_block.base_fee);
    const auto effective_gas_price = m_block.base_fee + priority_gas_price;

    return evmc_tx_context{
        intx::be::store<uint256be>(effective_gas_price),  // By EIP-1559.
        m_tx.sender,
        m_block.coinbase,
        m_block.number,
        m_block.timestamp,
        m_block.gas_limit,
        m_block.prev_randao,
        bytes32{m_tx.chain_id},
        uint256be{m_block.base_fee},
        intx::be::store<uint256be>(m_block.blob_base_fee.value_or(0)),
        m_tx.blob_hashes.data(),
        m_tx.blob_hashes.size(),
        nullptr,  // initcodes (TXCREATE) — unused here
        0,        // initcodes_count
        m_block.slot_number,
    };
}

bytes32 Host::get_block_hash(int64_t block_number) const noexcept
{
    return m_block_hashes.get_block_hash(block_number);
}

void Host::emit_log(const address& addr, const uint8_t* data, size_t data_size,
    const bytes32 topics[], size_t topics_count) noexcept
{
    m_logs.push_back({addr, {data, data_size}, {topics, topics + topics_count}});
}

void Host::emit_transfer_log(
    const address& sender, const address& recipient, const intx::uint256& amount) noexcept
{
    // EIP-7708: LOG3 from the system address with the ERC-20 Transfer topic.
    static constexpr auto SYSTEM_ADDRESS = 0xfffffffffffffffffffffffffffffffffffffffe_address;
    // keccak256("Transfer(address,address,uint256)")
    static constexpr auto TRANSFER_TOPIC =
        0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef_bytes32;

    if (amount == 0)
        return;

    bytes32 sender_topic{};
    bytes32 recipient_topic{};
    std::copy_n(sender.bytes, sizeof(sender.bytes), &sender_topic.bytes[12]);
    std::copy_n(recipient.bytes, sizeof(recipient.bytes), &recipient_topic.bytes[12]);

    const bytes32 topics[3]{TRANSFER_TOPIC, sender_topic, recipient_topic};
    const auto data = intx::be::store<bytes32>(amount);
    m_logs.push_back({SYSTEM_ADDRESS, {data.bytes, sizeof(data.bytes)},
        {topics, topics + std::size(topics)}});
}

evmc_access_status Host::access_account(const address& addr) noexcept
{
    if (m_rev < EVMC_BERLIN)
        return EVMC_ACCESS_COLD;  // Ignore before Berlin.

    auto& acc = m_state.get_or_insert(addr, {.erase_if_empty = true});

    if (acc.access_status == EVMC_ACCESS_WARM || is_precompile(m_rev, addr))
        return EVMC_ACCESS_WARM;

    m_state.journal_access_account(addr);
    acc.access_status = EVMC_ACCESS_WARM;
    return EVMC_ACCESS_COLD;
}

evmc_access_status Host::access_storage(const address& addr, const bytes32& key) noexcept
{
    auto& storage_slot = m_state.get_storage(addr, key);
    m_state.journal_storage_change(addr, key, storage_slot);
    return std::exchange(storage_slot.access_status, EVMC_ACCESS_WARM);
}


evmc::bytes32 Host::get_transient_storage(const address& addr, const bytes32& key) const noexcept
{
    const auto& acc = m_state.get(addr);
    const auto it = acc.transient_storage.find(key);
    return it != acc.transient_storage.end() ? it->second : bytes32{};
}

void Host::set_transient_storage(
    const address& addr, const bytes32& key, const bytes32& value) noexcept
{
    auto& slot = m_state.get(addr).transient_storage[key];
    m_state.journal_transient_storage_change(addr, key, slot);
    slot = value;
}
}  // namespace evmone::state
