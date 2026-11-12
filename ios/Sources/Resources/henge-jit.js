// StikDebug script that services WindowsiOS's JIT handshake on TXM devices (iOS 26 and later).
// The app (ios/Sources/Native/winios_jit.c) traps with:
//   brk #0x69   probe: answer with a nonzero x0;
//   brk #0xf00d with x16 = 1: x1 bytes of RX memory are wanted at x0, or anywhere when x0 is 0;
//               allocate them, bless every 16 KiB page, and hand the address back in x0 (0 on failure);
//   brk #0xf00d with x16 = 0: the JIT is live, so detach.
// Every handshake trap is resumed past its brk. A signal is passed on to the app; a Mach exception
// cannot be forwarded by resuming, so the script detaches and lets the app handle (or crash on) it.

const MAX_STOPS = 200;

const pid = get_pid();
const attach = send_command(`vAttach;${pid.toString(16)}`);
log(`attach pid ${pid}: ${attach}`);
if (/^T/.test(attach)) {
    serve();
} else {
    log('attach failed');
}

function serve() {
    let resume = 'c';
    for (let stops = 1; ; stops++) {
        if (stops > MAX_STOPS) {
            log(`giving up after ${MAX_STOPS} stops without a detach request`);
            detach();
            return;
        }
        const stop = send_command(resume);
        resume = 'c';
        if (!stop || /^[WXE]/.test(stop)) {
            log(`target is gone: ${stop}`);
            return;
        }
        const tid = (/thread:([0-9a-f]+);/.exec(stop) || [])[1];
        const pc = register(stop, '20');
        if (tid === undefined || pc === null) {
            log(`unreadable stop: ${stop}`);
            detach();
            return;
        }
        const word = send_command(`m${pc.toString(16)},4`);
        const insn = /^[0-9a-f]{8}$/.test(word) ? parseInt(word.match(/../g).reverse().join(''), 16) : 0;
        const imm = (insn >>> 5) & 0xffff;
        const isBrk = ((insn & 0xffe0001f) >>> 0) === 0xd4200000;
        if (!isBrk || (imm !== 0x69 && imm !== 0xf00d)) {
            const metype = (/;metype:([0-9a-f]+);/.exec(stop) || [])[1];
            const softSignal = metype === '5' && /;medata:10003;/.test(stop);
            if (metype !== undefined && !softSignal) {
                log(`exception ${metype} at insn ${word}: detaching so the app handles it`);
                detach();
                return;
            }
            const signal = (/^T([0-9a-f]{2})/.exec(stop) || [])[1] || '05';
            log(`signal 0x${signal}: passing it on`);
            resume = signal === '11' ? 'c' : `vCont;C${signal}:${tid};c`;
            continue;
        }
        if (!setRegister('20', pc + 4n, tid)) {
            detach();
            return;
        }
        if (imm === 0x69) {
            setRegister('0', 0xe0000069n, tid);
            log('probe answered');
            continue;
        }
        const command = register(stop, '10');
        if (command === 0n) {
            detach();
            return;
        }
        if (command === 1n) {
            // On failure the app sees x0 == 0 and gives up; detaching frees StikDebug at once.
            if (!prepareRegion(register(stop, '00'), register(stop, '01'), tid)) {
                detach();
                return;
            }
            continue;
        }
        log(`unknown command ${command}`);
    }
}

function detach() {
    log(`detach: ${send_command('D')}`);
}

function prepareRegion(want, size, tid) {
    if (want === null || size === null || size === 0n) {
        log('prepare: no size given');
        setRegister('0', 0n, tid);
        return false;
    }
    let addr = want;
    let allocated = false;
    if (addr === 0n) {
        const reply = send_command(`_M${size.toString(16)},rx`);
        if (!/^[0-9a-f]+$/.test(reply)) {
            log(`prepare: allocation failed: ${reply}`);
            setRegister('0', 0n, tid);
            return false;
        }
        addr = BigInt(`0x${reply}`);
        allocated = true;
    }
    // StikDebug's native writer takes plain integers; JavaScriptCore cannot pass a BigInt there.
    const blessed = prepare_memory_region(Number(addr), Number(size));
    if (blessed !== 'OK') {
        log(`prepare: blessing 0x${addr.toString(16)} failed: ${blessed}`);
        if (allocated) log(`free: ${send_command(`_m${addr.toString(16)}`)}`);
        setRegister('0', 0n, tid);
        return false;
    }
    if (!setRegister('0', addr, tid)) return false;
    log(`prepare: ${size >> 20n} MiB at 0x${addr.toString(16)}`);
    return true;
}

// Registers in a stop reply are `<gdb number>:<16 hex digits, little endian>;`.
function register(stop, number) {
    const m = new RegExp(`(?:^T[0-9a-f]{2}|;)${number}:([0-9a-f]{16});`).exec(stop);
    return m ? BigInt(`0x${m[1].match(/../g).reverse().join('')}`) : null;
}

function setRegister(number, value, tid) {
    let hex = '';
    for (let i = 0; i < 8; i++) {
        hex += Number(value & 0xffn).toString(16).padStart(2, '0');
        value >>= 8n;
    }
    const reply = send_command(`P${number}=${hex};thread:${tid};`);
    if (reply !== 'OK') log(`P${number} failed: ${reply}`);
    return reply === 'OK';
}
