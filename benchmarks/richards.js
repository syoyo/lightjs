// Richards Benchmark
// Classic object-oriented benchmark for method dispatch and object allocation

class Packet {
  constructor(link, identity, kind) {
    this.link = link;
    this.identity = identity;
    this.kind = kind;
    this.datum = 0;
    this.data = [];
  }
}

class TaskControlBlock {
  constructor(link, identity, priority, initialWorkQueue, initialState, privateData) {
    this.link = link;
    this.identity = identity;
    this.priority = priority;
    this.workQueue = initialWorkQueue;
    this.state = initialState;
    this.privateData = privateData;
  }

  setRunning() {
    this.state = 1;
  }

  markAsNotHeld() {
    this.state = this.state & 0xFFFE;
  }

  isHeldOrSuspended() {
    return (this.state & 1) !== 0 || this.state === 0;
  }

  isWaitingWithPacket() {
    return (this.state & 2) !== 0;
  }

  run() {
    let packet;
    if (this.isWaitingWithPacket()) {
      packet = this.workQueue;
      this.workQueue = packet.link;
      if (this.workQueue === null) {
        this.setRunning();
      } else {
        this.markAsNotHeld();
      }
    } else {
      packet = null;
    }
    return this.privateData.run(packet);
  }
}

class IdleTask {
  constructor() {
    this.control = 1;
    this.count = 10000;
  }

  run(packet) {
    this.count = this.count - 1;
    if (this.count === 0) {
      return null;
    }
    if ((this.control & 1) === 0) {
      this.control = this.control / 2;
      return null;
    }
    this.control = (this.control / 2) ^ 0xD008;
    return null;
  }
}

class WorkerTask {
  constructor() {
    this.destination = 0;
    this.count = 0;
  }

  run(packet) {
    if (packet === null) {
      return null;
    }
    this.destination = (this.destination + 1) % 2;
    packet.identity = this.destination;
    packet.datum = 0;
    for (let i = 0; i < 4; i++) {
      this.count = this.count + 1;
      if (this.count > 26) {
        this.count = 1;
      }
      packet.data[i] = this.count;
    }
    return packet;
  }
}

class HandlerTask {
  constructor() {
    this.workIn = null;
    this.deviceIn = null;
  }

  run(packet) {
    if (packet !== null) {
      if (packet.kind === 1) {
        this.workIn = this.append(packet, this.workIn);
      } else {
        this.deviceIn = this.append(packet, this.deviceIn);
      }
    }
    if (this.workIn !== null) {
      let count = this.workIn.datum;
      if (count < 4) {
        if (this.deviceIn !== null) {
          let dev = this.deviceIn;
          this.deviceIn = this.deviceIn.link;
          dev.datum = this.workIn.data[count];
          this.workIn.datum = count + 1;
          return dev;
        }
      } else {
        this.workIn = this.workIn.link;
      }
    }
    return null;
  }

  append(packet, queue) {
    packet.link = null;
    if (queue === null) {
      return packet;
    }
    let mouse = queue;
    while (mouse.link !== null) {
      mouse = mouse.link;
    }
    mouse.link = packet;
    return queue;
  }
}

class DeviceTask {
  constructor() {
    this.pending = null;
  }

  run(packet) {
    if (packet === null) {
      if (this.pending === null) {
        return null;
      }
      let v = this.pending;
      this.pending = null;
      return v;
    }
    this.pending = packet;
    return null;
  }
}

// Run Richards benchmark
let queue = null;

function addTask(id, priority, queue, task) {
  const tcb = new TaskControlBlock(null, id, priority, queue, queue === null ? 1 : 2, task);
  return tcb;
}

function schedule() {
  let current = queue;
  while (current !== null) {
    if (current.isHeldOrSuspended()) {
      current = current.link;
    } else {
      current = current.run();
    }
  }
}

// Build task queue
let idleTask = addTask(0, 0, null, new IdleTask());
queue = idleTask;

let packet = new Packet(null, 0, 1);
packet = new Packet(packet, 0, 1);
let workerTask = addTask(1, 1000, packet, new WorkerTask());
workerTask.link = idleTask;
queue = workerTask;

packet = new Packet(null, 1, 2);
packet = new Packet(packet, 1, 2);
packet = new Packet(packet, 1, 2);
let handlerA = addTask(2, 2000, packet, new HandlerTask());
handlerA.link = workerTask;
queue = handlerA;

packet = new Packet(null, 2, 2);
packet = new Packet(packet, 2, 2);
packet = new Packet(packet, 2, 2);
let handlerB = addTask(3, 3000, packet, new HandlerTask());
handlerB.link = handlerA;
queue = handlerB;

let deviceA = addTask(4, 4000, null, new DeviceTask());
deviceA.link = handlerB;
queue = deviceA;

let deviceB = addTask(5, 5000, null, new DeviceTask());
deviceB.link = deviceA;
queue = deviceB;

schedule();

console.log("Richards benchmark completed");
