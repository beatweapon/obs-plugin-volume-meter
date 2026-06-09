const meters = document.getElementById("meters");
const statusText = document.getElementById("status");
const meterElements = new Map();

const PEAK_HOLD_TIME_MS = 20000; // ピークライン表示時間（ミリ秒）
const YELLOW_THRESHOLD_DB = -20; // 黄色表示のしきい値
const RED_THRESHOLD_DB = -9; // 赤色表示のしきい値
const MIN_DB = -60;
const MAX_DB = 0;

function dbToPercent(db) {
  if (db === null || db === undefined || !Number.isFinite(db)) {
    return 0;
  }

  const clamped = Math.max(MIN_DB, Math.min(MAX_DB, db));
  return ((clamped - MIN_DB) / (MAX_DB - MIN_DB)) * 100;
}

document.documentElement.style.setProperty(
  "--yellow-pos",
  `${dbToPercent(YELLOW_THRESHOLD_DB)}%`,
);

document.documentElement.style.setProperty(
  "--red-pos",
  `${dbToPercent(RED_THRESHOLD_DB)}%`,
);

function getMeterElement(source) {
  let item = meterElements.get(source.uuid);
  if (item) {
    if (item.sourceName !== source.name) {
      item.sourceName = source.name;
      sortMeters();
    }

    return item;
  }

  item = {
    container: document.createElement("section"),
    channels: new Map(),
    lastPeakTime: {},
    sourceName: source.name,
  };
  item.container.className = "meter";

  const header = document.createElement("div");
  header.className = "meter-header";
  header.innerHTML = `<div class="name"></div>`;
  item.container.appendChild(header);

  const channelsDiv = document.createElement("div");
  channelsDiv.className = "channels";
  item.container.appendChild(channelsDiv);
  item.channelsContainer = channelsDiv;

  meters.appendChild(item.container);
  sortMeters();
  meterElements.set(source.uuid, item);
  return item;
}

const sortMeters = () => {
  const items = [...meterElements.values()];

  items.sort((a, b) => a.sourceName.localeCompare(b.sourceName, "ja"));

  for (const item of items) {
    meters.appendChild(item.container);
  }
};

function getChannelElement(meterItem, channelIndex, source) {
  if (!meterItem.channels.has(channelIndex)) {
    const channelDiv = document.createElement("div");
    channelDiv.className = "channel";
    channelDiv.innerHTML = `
			<div class="bar">
				<div class="fill"></div>
				<div class="magnitude-line"></div>
				<div class="peak"></div>
			</div>
			<div class="channel-label">Ch${channelIndex + 1}</div>
		`;
    meterItem.channelsContainer.appendChild(channelDiv);
    meterItem.channels.set(channelIndex, {
      container: channelDiv,
      fill: channelDiv.querySelector(".fill"),
      magnitudeLine: channelDiv.querySelector(".magnitude-line"),
      peak: channelDiv.querySelector(".peak"),
      lastPeakDb: -Infinity,
      peakHoldStart: 0,
      clippingStart: 0,
      fillHeight: 0,
      lastUpdateTime: 0,
    });
  }
  return meterItem.channels.get(channelIndex);
}

function render(payload) {
  const seen = new Set();
  const now = Date.now();

  for (const source of payload.sources ?? []) {
    // チャンネルが空のソースはスキップ
    if (!source.channels || source.channels.length === 0) {
      continue;
    }

    seen.add(source.uuid);
    const meterItem = getMeterElement(source);

    // メーターヘッダーの更新
    const header = meterItem.container.querySelector(".meter-header");
    header.querySelector(".name").textContent = source.name;

    // ピーク値の計算（全チャンネルの最大値）
    let maxPeakDb = -Infinity;
    for (const channel of source.channels ?? []) {
      if (Number.isFinite(channel.peak)) {
        maxPeakDb = Math.max(maxPeakDb, channel.peak);
      }
    }

    // チャンネル削除: 古いチャンネルを削除
    const currentChannelCount = source.channels?.length ?? 0;
    for (const [chIndex, chElement] of meterItem.channels) {
      if (chIndex >= currentChannelCount) {
        chElement.container.remove();
        meterItem.channels.delete(chIndex);
      }
    }

    // チャンネルの更新
    for (let ch = 0; ch < (source.channels?.length ?? 0); ++ch) {
      const channel = source.channels[ch];
      const chElement = getChannelElement(meterItem, ch, source);

      const magnitude = channel.magnitude ?? -Infinity;
      const peak = channel.peak ?? -Infinity;

      // マグニチュード（現在値）を黒いラインで表現
      const magnitude_percent = Math.min(
        100,
        Math.max(0, dbToPercent(magnitude)),
      );
      chElement.magnitudeLine.style.setProperty(
        "--magnitude",
        `${magnitude_percent}%`,
      );

      // ピークに基づいてフィル高さを更新
      const peakPercent = Math.min(100, Math.max(0, dbToPercent(peak)));
      const targetHeight = peakPercent;

      const deltaTime =
        chElement.lastUpdateTime > 0
          ? (now - chElement.lastUpdateTime) / 1000
          : 0;

      chElement.lastUpdateTime = now;

      if (targetHeight > chElement.fillHeight) {
        // 上昇は即時
        chElement.fillHeight = targetHeight;
      } else {
        // 下降はゆっくり
        const decayPerSecond = 40;
        chElement.fillHeight = Math.max(
          targetHeight,
          chElement.fillHeight - decayPerSecond * deltaTime,
        );
      }

      // フィルの高さを適用
      chElement.fill.style.setProperty("--level", `${chElement.fillHeight}%`);

      // ピークの更新と表示時間管理
      if (Number.isFinite(peak) && peak !== null) {
        // ピーク値が更新された場合
        if (
          !Number.isFinite(chElement.lastPeakDb) ||
          peak > chElement.lastPeakDb
        ) {
          chElement.lastPeakDb = peak;
          chElement.peakHoldStart = now;

          // ピークが0dBを超えた場合、クリッピングを記録
          if (peak > MAX_DB) {
            chElement.clippingStart = now;
          }
        }
      }

      // ピークラインの表示判定と20秒ごとのリセット
      const timeSincePeak = now - chElement.peakHoldStart;
      if (timeSincePeak >= PEAK_HOLD_TIME_MS) {
        // 20秒経過した場合、ピークホールドをリセット
        if (Number.isFinite(chElement.lastPeakDb)) {
          chElement.lastPeakDb = -Infinity;
          chElement.peakHoldStart = now;
        }
      }

      // ピークラインを表示
      if (
        Number.isFinite(chElement.lastPeakDb) &&
        now - chElement.peakHoldStart < PEAK_HOLD_TIME_MS
      ) {
        const peakPercent = Math.min(
          100,
          Math.max(0, dbToPercent(chElement.lastPeakDb)),
        );
        chElement.peak.style.setProperty("--peak", `${peakPercent}%`);
      } else {
        chElement.peak.style.setProperty("--peak", `-100%`);
      }

      // クリッピング状態の管理（5秒間ゲージ全体を赤くする）
      const timeSinceClipping = now - chElement.clippingStart;
      if (timeSinceClipping < 1000) {
        chElement.fill.classList.add("clipping");
      } else {
        chElement.fill.classList.remove("clipping");
      }
    }
  }

  // 削除されたメーターを削除
  for (const [uuid, item] of meterElements) {
    if (!seen.has(uuid)) {
      item.container.remove();
      meterElements.delete(uuid);
    }
  }
}

function connect() {
  const socket = new WebSocket("ws://127.0.0.1:4457");

  socket.addEventListener("open", () => {
    document.body.classList.add("connected");
  });

  socket.addEventListener("message", (event) => {
    try {
      const data = JSON.parse(event.data);
      console.log("Received data:", data);
      render(data);
    } catch (e) {
      console.error("Parse error:", e, "Raw data:", event.data);
      statusText.textContent = "Received invalid meter data.";
    }
  });

  socket.addEventListener("close", () => {
    document.body.classList.remove("connected");
    statusText.textContent = "Disconnected. Reconnecting…";
    setTimeout(connect, 1000);
  });

  socket.addEventListener("error", () => {
    statusText.textContent = "Connection failed. Is OBS plugin loaded?";
    socket.close();
  });
}

connect();
