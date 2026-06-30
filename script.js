// ──────────────────────────────────────────────────────────────────────────────
// CONFIGURAÇÃO MQTT (WebSocket)
// ──────────────────────────────────────────────────────────────────────────────
const BROKER_WS  = 'wss://broker.hivemq.com:8884/mqtt'; // WebSocket seguro
let lixeiraId    = 'lixeira_01';
let mqttClient   = null;
let travado      = false;

// ── Helpers ──────────────────────────────────────────────────────────────────
function agora() {
  return new Date().toLocaleTimeString('pt-BR', { hour:'2-digit', minute:'2-digit', second:'2-digit' });
}

function corNivel(pct) {
  if (pct >= 95) return 'var(--red)';
  if (pct >= 75) return 'var(--yellow)';
  return 'var(--green)';
}

function textoCaption(media) {
  if (media >= 95) return '🔴 Cheio — esvazie imediatamente';
  if (media >= 75) return '🟡 Quase cheio — atenção necessária';
  if (media >= 40) return '🟢 Parcialmente ocupado';
  return '🟢 Espaço disponível';
}

// ── Gauge ─────────────────────────────────────────────────────────────────────
function atualizarGauge(pct) {
  const total = 220;
  const offset = total - (pct / 100) * total;
  const fill = document.getElementById('gauge-fill');
  fill.style.strokeDashoffset = offset;
  fill.style.stroke = corNivel(pct);
  document.getElementById('gauge-val').textContent = pct;
  document.getElementById('gauge-caption').textContent = textoCaption(pct);
}

// ── Nível bars ────────────────────────────────────────────────────────────────
function atualizarNivel(id_pct, id_bar, pct) {
  document.getElementById(id_pct).textContent = pct + '%';
  const bar = document.getElementById(id_bar);
  bar.style.width  = pct + '%';
  bar.style.background = corNivel(pct);
}

// ── Feed de eventos ───────────────────────────────────────────────────────────
function adicionarEvento(badgeClass, badgeText, mensagem) {
  const lista = document.getElementById('feed-list');
  const li = document.createElement('li');
  li.className = 'feed-item';
  li.innerHTML = `
    <span class="feed-time">${agora()}</span>
    <span class="feed-badge ${badgeClass}">${badgeText}</span>
    <span class="feed-msg">${mensagem}</span>
  `;
  lista.insertBefore(li, lista.firstChild);
  // Limita em 50 eventos
  while (lista.children.length > 50) lista.removeChild(lista.lastChild);
}

// ── Banner de alerta ──────────────────────────────────────────────────────────
function mostrarAlerta(tipo, mensagem) {
  const banner = document.getElementById('alerta-banner');
  document.getElementById('alerta-msg').textContent = mensagem;
  banner.className = (tipo === 'critico') ? 'critico' : 'aviso';

  if (tipo === 'limpar') {
    banner.style.display = 'none';
    banner.className = '';
  }
}

// ── Conexão badge ─────────────────────────────────────────────────────────────
function setConexao(ok) {
  const badge = document.getElementById('conn-badge');
  const label = document.getElementById('conn-label');
  const btns  = document.querySelectorAll('.controls .btn');
  badge.className = 'conn-badge ' + (ok ? 'connected' : 'disconnected');
  label.textContent = ok ? 'Online · ' + lixeiraId : 'Desconectado';
  btns.forEach(b => b.disabled = !ok);
}

// ── Processar mensagem de STATUS ──────────────────────────────────────────────
function processarStatus(data) {
  // Sensores
  document.getElementById('sensor-umid').innerHTML    = data.umidade    + '<span class="sensor-unit">%</span>';
  document.getElementById('sensor-gatilho').innerHTML = data.gatilho_cm + '<span class="sensor-unit">cm</span>';
  document.getElementById('sensor-servo').innerHTML   = data.servo_graus + '<span class="sensor-unit">°</span>';

  // Stats
  document.getElementById('stat-total').textContent  = data.total_triagens ?? '—';
  document.getElementById('stat-umidos').textContent = data.total_umidos   ?? '—';
  document.getElementById('stat-secos').textContent  = data.total_secos    ?? '—';

  // Níveis
  const dir = data.nivel_dir_pct ?? 0;
  const esq = data.nivel_esq_pct ?? 0;
  atualizarNivel('nivel-dir-pct', 'nivel-dir-bar', dir);
  atualizarNivel('nivel-esq-pct', 'nivel-esq-bar', esq);

  // Gauge (média)
  const media = Math.round((dir + esq) / 2);
  atualizarGauge(media);

  // Alertas automáticos por nível
  if (media >= 95) {
    mostrarAlerta('critico', '⚠ Lixeira cheia! Sistema travado automaticamente.');
  } else if (media >= 75) {
    mostrarAlerta('aviso', '⚠ Lixeira quase cheia — considere esvaziar em breve.');
  } else {
    mostrarAlerta('limpar', '');
  }

  // Estado de trava
  travado = data.travado;
}

// ── Processar mensagem de TRIAGEM ─────────────────────────────────────────────
function processarTriagem(data) {
  const umido = data.tipo === 'UMIDO';
  adicionarEvento(
    umido ? 'badge-umido' : 'badge-seco',
    data.tipo,
    `Descartado para ${umido ? 'direita (úmido)' : 'esquerda (seco)'} · Umidade: ${data.umidade_pct}%`
  );
}

// ── Processar ALERTA ──────────────────────────────────────────────────────────
function processarAlerta(data) {
  const tiposCriticos = ['CHEIO_CRITICO', 'ENTRADA_BLOQUEADA'];
  const critico = tiposCriticos.includes(data.tipo);

  adicionarEvento(
    'badge-alerta',
    data.tipo,
    data.msg
  );

  if (critico) {
    mostrarAlerta('critico', '⚠ ' + data.msg);
  } else if (data.tipo === 'RESET') {
    adicionarEvento('badge-cmd', 'RESET', 'Contadores zerados');
  }
}

// ── MQTT ──────────────────────────────────────────────────────────────────────
function conectar(id) {
  lixeiraId = id.trim() || 'lixeira_01';
  document.getElementById('lixeira-id-display').textContent = 'lixeira/' + lixeiraId;

  if (mqttClient && mqttClient.isConnected()) {
    mqttClient.disconnect();
  }

  const clientId = 'dashboard_' + Math.random().toString(16).slice(2, 10);
  mqttClient = new Paho.MQTT.Client(BROKER_WS, clientId);

  mqttClient.onConnectionLost = (resp) => {
    setConexao(false);
    adicionarEvento('badge-cmd', 'MQTT', 'Conexão perdida: ' + resp.errorMessage);
    setTimeout(() => reconectar(), 5000);
  };

  mqttClient.onMessageArrived = (msg) => {
    try {
      const data = JSON.parse(msg.payloadString);
      const topico = msg.destinationName;

      if (topico.includes('/status'))  processarStatus(data);
      if (topico.includes('/triagem')) processarTriagem(data);
      if (topico.includes('/alerta'))  processarAlerta(data);
    } catch (e) {
      console.warn('Payload inválido:', msg.payloadString);
    }
  };

  mqttClient.connect({
    useSSL: true,
    onSuccess: () => {
      setConexao(true);
      mqttClient.subscribe('lixeira/' + lixeiraId + '/status');
      mqttClient.subscribe('lixeira/' + lixeiraId + '/triagem');
      mqttClient.subscribe('lixeira/' + lixeiraId + '/alerta');
      adicionarEvento('badge-cmd', 'MQTT', 'Conectado ao broker · monitorando ' + lixeiraId);
    },
    onFailure: (err) => {
      setConexao(false);
      adicionarEvento('badge-cmd', 'ERRO', 'Falha na conexão: ' + err.errorMessage);
    }
  });
}

function reconectar() {
  const id = document.getElementById('id-input').value;
  conectar(id);
}

function enviarCmd(cmd) {
  if (!mqttClient || !mqttClient.isConnected()) return;
  const msg = new Paho.MQTT.Message(cmd);
  msg.destinationName = 'lixeira/' + lixeiraId + '/cmd';
  mqttClient.send(msg);
  adicionarEvento('badge-cmd', 'CMD', 'Comando enviado: ' + cmd.toUpperCase());
}

// ── Init ──────────────────────────────────────────────────────────────────────
window.addEventListener('load', () => conectar('lixeira_01'));