let weatherLocation = null, weatherController = null, weatherTimer = null;
let weatherGeneration = 0, hasWeather = false;

async function fetchTimed(url, options = {}) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 15000);
  try { return await fetch(url, {...options, signal:controller.signal}); }
  finally { clearTimeout(timeout); }
}

function validLocation(value) {
  return value && typeof value.name === 'string' && value.name.trim() &&
    Number.isFinite(value.latitude) && Math.abs(value.latitude) <= 90 &&
    Number.isFinite(value.longitude) && Math.abs(value.longitude) <= 180;
}

function weatherText(code) {
  if (code === 0) return '晴朗';
  if ([1,2,3].includes(code)) return ['晴時多雲','局部多雲','陰天'][code-1];
  if ([45,48].includes(code)) return '有霧';
  if ([51,53,55,56,57].includes(code)) return '毛毛雨';
  if ([61,63,65,66,67].includes(code)) return '下雨';
  if ([71,73,75,77,85,86].includes(code)) return '降雪';
  if ([80,81,82].includes(code)) return '陣雨';
  if ([95,96,99].includes(code)) return '雷雨';
  return '天氣狀態未知';
}

function applyLocation(next) {
  if (!validLocation(next)) throw Error('Invalid location');
  if (weatherLocation && next.name === weatherLocation.name &&
      next.latitude === weatherLocation.latitude && next.longitude === weatherLocation.longitude) return;
  weatherLocation = next;
  weatherGeneration++;
  if (weatherController) weatherController.abort();
  clearTimeout(weatherTimer);
  hasWeather = false;
  el('weatherTitle').textContent = next.name + ' · 網路天氣';
  el('weatherSummary').textContent = '讀取中…';
  el('weatherStatus').textContent = '正在取得所選地區的天氣';
  ['temperature','humidity','windSpeed','windDirection','weatherTime'].forEach(id => el(id).textContent = '--');
  refreshWeather();
}

async function refreshLocation() {
  try {
    const response = await fetchTimed('/api/weather-location', {cache:'no-store'});
    if (!response.ok) throw Error('Location unavailable');
    const next = await response.json();
    applyLocation(next);
  } catch (error) {
    if (!weatherLocation) el('weatherSummary').textContent = '等待地區設定';
  } finally { setTimeout(refreshLocation, 5000); }
}

async function refreshWeather() {
  if (!weatherLocation) return;
  const generation = weatherGeneration, location = weatherLocation;
  const controller = new AbortController();
  weatherController = controller;
  const timeout = setTimeout(() => controller.abort(), 15000);
  try {
    const params = new URLSearchParams({latitude:location.latitude,longitude:location.longitude,
      current:'temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m',
      wind_speed_unit:'ms',timezone:'Asia/Taipei'});
    const response = await fetch('https://api.open-meteo.com/v1/forecast?' + params, {signal:controller.signal,cache:'no-store'});
    if (!response.ok) throw Error('Weather HTTP ' + response.status);
    const data = await response.json(), c = data.current;
    if (generation !== weatherGeneration) return;
    if (!c || !['temperature_2m','relative_humidity_2m','weather_code','wind_speed_10m','wind_direction_10m'].every(key => Number.isFinite(c[key])) || typeof c.time !== 'string') throw Error('Incomplete weather');
    el('weatherSummary').textContent = weatherText(c.weather_code);
    el('temperature').textContent = c.temperature_2m.toFixed(1) + ' °C';
    el('humidity').textContent = c.relative_humidity_2m + ' %';
    el('windSpeed').textContent = c.wind_speed_10m.toFixed(1) + ' m/s';
    el('windDirection').textContent = ['北','東北','東','東南','南','西南','西','西北'][Math.round(c.wind_direction_10m / 45) % 8] + '（' + c.wind_direction_10m + '°）';
    el('weatherTime').textContent = c.time.replace('T',' ');
    el('weatherStatus').textContent = '已取得天氣資料';
    hasWeather = true;
  } catch (error) {
    if (generation !== weatherGeneration) return;
    if (!hasWeather) el('weatherSummary').textContent = '暫時無法取得天氣';
    el('weatherStatus').textContent = hasWeather ? '更新失敗，保留此地區上次資料，請留意資料時間。' : '請確認手機／電腦可連上網際網路，10 分鐘後重試。';
  } finally {
    clearTimeout(timeout);
    if (generation === weatherGeneration) weatherTimer = setTimeout(refreshWeather, 600000);
  }
}

refreshLocation();
