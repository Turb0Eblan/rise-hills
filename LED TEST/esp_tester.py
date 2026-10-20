"""
ESP32 LED Master Controller
PyQt6 desktop app for EliteHome LED system
"""

import json
import serial
import serial.tools.list_ports
import serial.threaded
import sys
import faulthandler
import signal
from datetime import datetime

from PyQt6.QtCore import Qt, pyqtSignal, QObject, QThread, QTimer
from PyQt6.QtGui import QFont, QColor, QPalette, QIcon
from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QComboBox, QPushButton, QLabel, QSpinBox, QTextEdit, QGroupBox,
    QGridLayout, QMessageBox, QLineEdit, QFrame, QDialog, QTabWidget,
    QScrollArea, QSizePolicy, QSlider, QCheckBox, QFormLayout,
    QTableWidget, QTableWidgetItem, QHeaderView, QSplitter,
    QAbstractItemView
)

if sys.stderr:
    faulthandler.enable()
    for _sig in (signal.SIGABRT, signal.SIGSEGV, signal.SIGTERM):
        try:
            faulthandler.register(_sig, all_threads=True)
        except Exception:
            pass
def _qt_exception_hook(exc_type, exc_value, exc_tb):
    """Prevent PyQt6 from calling abort() on unhandled slot exceptions."""
    import traceback
    traceback.print_exception(exc_type, exc_value, exc_tb)

sys.excepthook = _qt_exception_hook

DEVICES = ["yellow", "red", "blue", "orange", "green", "all"]
COLORS  = ["white", "red", "green", "blue", "yellow", "cyan", "magenta", "orange", "purple", "warm", "off"]
EFFECTS = ["blink", "breath", "wipe", "chase", "sparkle", "strobe"]

def _apt_id(apt: dict) -> int:
    """Read apartment ID regardless of key casing (aptID / aptId / id / number)."""
    for k in ("aptID", "aptId", "aptid", "id", "number", "apt_id"):
        if k in apt:
            try: return int(apt[k])
            except (ValueError, TypeError): pass
    return 0

def _apt_range(apt: dict) -> list:
    """Read apartment LED range regardless of key casing."""
    for k in ("aptRange", "aptrange", "ledRange", "range"):
        if k in apt:
            return apt[k]
    return [0, 0]


DARK_STYLE = """
QMainWindow, QDialog, QWidget {
    background-color: #0f0f14;
    color: #e0e0e0;
    font-family: 'Consolas', 'Courier New', monospace;
    font-size: 12px;
}
QGroupBox {
    border: 1px solid #2a2a3a;
    border-radius: 6px;
    margin-top: 10px;
    padding-top: 8px;
    font-weight: bold;
    color: #7878ff;
    font-size: 11px;
    letter-spacing: 1px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 6px;
}
QPushButton {
    background-color: #1a1a2e;
    color: #c0c0ff;
    border: 1px solid #3a3a5a;
    border-radius: 4px;
    padding: 5px 10px;
    font-family: 'Consolas', monospace;
    font-size: 11px;
}
QPushButton:hover {
    background-color: #25255a;
    border-color: #6060cc;
    color: #ffffff;
}
QPushButton:pressed {
    background-color: #4040aa;
}
QPushButton#btn_off {
    color: #ff6060;
    border-color: #661111;
}
QPushButton#btn_off:hover {
    background-color: #3a1010;
    border-color: #ff4444;
}
QPushButton#btn_on {
    color: #60ff60;
    border-color: #116611;
}
QPushButton#btn_on:hover {
    background-color: #103a10;
    border-color: #44ff44;
}
QPushButton#btn_random {
    color: #ffcc44;
    border-color: #664400;
}
QPushButton#btn_send {
    background-color: #1e3a6e;
    color: #88bbff;
    border-color: #336699;
    font-weight: bold;
}
QPushButton#btn_send:hover {
    background-color: #2a4e9a;
    border-color: #5588cc;
}
QComboBox {
    background-color: #1a1a2e;
    color: #c0c0ff;
    border: 1px solid #3a3a5a;
    border-radius: 4px;
    padding: 3px 8px;
    min-width: 90px;
}
QComboBox::drop-down { border: none; width: 20px; }
QComboBox QAbstractItemView {
    background-color: #1a1a2e;
    color: #c0c0ff;
    selection-background-color: #3a3a7a;
}
QSpinBox, QLineEdit {
    background-color: #1a1a2e;
    color: #c0c0ff;
    border: 1px solid #3a3a5a;
    border-radius: 4px;
    padding: 3px 6px;
}
QTextEdit {
    background-color: #0a0a12;
    color: #88ff88;
    border: 1px solid #2a2a3a;
    border-radius: 4px;
    font-family: 'Consolas', 'Courier New', monospace;
    font-size: 11px;
}
QTabWidget::pane {
    border: 1px solid #2a2a3a;
    border-radius: 4px;
}
QTabBar::tab {
    background: #1a1a2e;
    color: #8080cc;
    padding: 6px 16px;
    border: 1px solid #2a2a3a;
    border-bottom: none;
    border-top-left-radius: 4px;
    border-top-right-radius: 4px;
}
QTabBar::tab:selected {
    background: #25254a;
    color: #c0c0ff;
    border-color: #5050aa;
}
QLabel { color: #a0a0cc; }
QLabel#status_connected { color: #44ff44; font-weight: bold; }
QLabel#status_disconnected { color: #ff4444; font-weight: bold; }
QSlider::groove:horizontal {
    height: 4px;
    background: #2a2a3a;
    border-radius: 2px;
}
QSlider::handle:horizontal {
    background: #6666cc;
    width: 14px; height: 14px;
    border-radius: 7px;
    margin: -5px 0;
}
QSlider::sub-page:horizontal {
    background: #4444aa;
    border-radius: 2px;
}
QTableWidget {
    background-color: #0e0e1a;
    color: #c0c0ff;
    gridline-color: #2a2a3a;
    border: 1px solid #2a2a3a;
}
QTableWidget::item:selected {
    background-color: #3a3a7a;
}
QHeaderView::section {
    background-color: #1a1a2e;
    color: #8080cc;
    border: 1px solid #2a2a3a;
    padding: 4px;
    font-size: 11px;
}
QScrollBar:vertical {
    background: #0f0f14;
    width: 8px;
    border-radius: 4px;
}
QScrollBar::handle:vertical {
    background: #3a3a5a;
    border-radius: 4px;
    min-height: 20px;
}
QFrame#separator {
    background-color: #2a2a3a;
    max-height: 1px;
}
"""

# ─── Serial Worker ────────────────────────────────────────────
class SerialWorker(QObject):
    data_received    = pyqtSignal(str)
    connection_status = pyqtSignal(bool)

    def __init__(self):
        super().__init__()
        self.serial_port = None
        self.reader      = None
        self.is_running  = False

    def connect(self, port, baudrate):
        try:
            self.serial_port = serial.Serial(port, baudrate, timeout=1)

            class Proto(serial.threaded.Protocol):
                def __init__(self, w):
                    super().__init__()
                    self.w = w
                def connection_made(self, _):
                    try: self.w.connection_status.emit(True)
                    except Exception: pass
                def data_received(self, data):
                    try:
                        text = data.decode('utf-8', errors='ignore') if isinstance(data, bytes) else str(data)
                        self.w.data_received.emit(text)
                    except Exception: pass
                def connection_lost(self, _):
                    try: self.w.connection_status.emit(False)
                    except Exception: pass

            self.reader = serial.threaded.ReaderThread(self.serial_port, lambda: Proto(self))
            self.reader.start()
            try: self.reader.connect()
            except Exception: pass
            self.is_running = True
        except Exception as e:
            self.connection_status.emit(False)
            self.data_received.emit(f"[ERROR] Connection failed: {e}\n")

    def send(self, command):
        try:
            if self.serial_port and self.serial_port.is_open:
                self.serial_port.write((command + '\n').encode())
                self.data_received.emit(f"[TX] {command}\n")
        except Exception as e:
            self.data_received.emit(f"[ERROR] {e}\n")

    def disconnect(self):
        self.is_running = False
        try:
            if self.reader:
                self.reader.stop()
                self.reader = None
        except Exception: pass
        try:
            if self.serial_port:
                self.serial_port.close()
                self.serial_port = None
        except Exception: pass
        try: self.connection_status.emit(False)
        except Exception: pass

# ─── Building JSON Editor Dialog ──────────────────────────────
class BuildingEditorDialog(QDialog):
    """
    Visual + Raw-JSON editor for the new nested structure:
      block -> floors[] -> { floor, floorRange, apartments[] -> { aptID, aptRange } }
    """
    def __init__(self, data, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Building Configuration Editor")
        self.setGeometry(150, 150, 1000, 680)
        self.data = json.loads(json.dumps(data))  # deep copy
        self._build_ui()

    def _build_ui(self):
        layout = QVBoxLayout(self)
        tabs = QTabWidget()
        tabs.addTab(self._make_visual_tab(), "Visual Editor")
        tabs.addTab(self._make_json_tab(),   "Raw JSON")
        layout.addWidget(tabs)
        btn_row = QHBoxLayout()
        btn_save = QPushButton("💾  Save & Close")
        btn_save.setObjectName("btn_send")
        btn_save.clicked.connect(self._save)
        btn_cancel = QPushButton("Cancel")
        btn_cancel.clicked.connect(self.reject)
        btn_row.addStretch()
        btn_row.addWidget(btn_cancel)
        btn_row.addWidget(btn_save)
        layout.addLayout(btn_row)

    # ── Visual tab ────────────────────────────────────────────
    def _make_visual_tab(self):
        w = QWidget()
        root = QHBoxLayout(w)

        # ── Left: block list ─────────────────────────────────
        left = QVBoxLayout()
        left.addWidget(QLabel("BLOCKS"))
        self.block_list = QTableWidget(0, 2)
        self.block_list.setHorizontalHeaderLabels(["ID", "Slave"])
        self.block_list.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        self.block_list.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.block_list.setMaximumWidth(200)
        self.block_list.itemSelectionChanged.connect(self._on_block_selected)
        left.addWidget(self.block_list)
        blk_btns = QHBoxLayout()
        b_add = QPushButton("+ Block"); b_add.clicked.connect(self._add_block)
        b_del = QPushButton("- Block"); b_del.setObjectName("btn_off"); b_del.clicked.connect(self._del_block)
        blk_btns.addWidget(b_add); blk_btns.addWidget(b_del)
        left.addLayout(blk_btns)
        root.addLayout(left)

        sep = QFrame(); sep.setFrameShape(QFrame.Shape.VLine)
        sep.setStyleSheet("color:#2a2a3a;"); root.addWidget(sep)

        # ── Middle: floor list ────────────────────────────────
        mid = QVBoxLayout()
        self.block_slave_label = QLabel("← Select block")
        mid.addWidget(self.block_slave_label)
        self.slave_combo = QComboBox()
        self.slave_combo.addItems(DEVICES[:-1])
        self.slave_combo.currentTextChanged.connect(self._update_slave)
        self.slave_combo.hide()
        mid.addWidget(self.slave_combo)
        mid.addWidget(QLabel("FLOORS"))
        self.floor_list = QTableWidget(0, 3)
        self.floor_list.setHorizontalHeaderLabels(["Floor #", "LED Start", "LED End"])
        self.floor_list.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        self.floor_list.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.floor_list.setMinimumWidth(260)
        self.floor_list.cellChanged.connect(self._floor_changed)
        self.floor_list.itemSelectionChanged.connect(self._on_floor_selected)
        mid.addWidget(self.floor_list)
        fl_btns = QHBoxLayout()
        bf_add = QPushButton("+ Floor"); bf_add.clicked.connect(self._add_floor)
        bf_del = QPushButton("- Floor"); bf_del.setObjectName("btn_off"); bf_del.clicked.connect(self._del_floor)
        fl_btns.addWidget(bf_add); fl_btns.addWidget(bf_del)
        mid.addLayout(fl_btns)
        root.addLayout(mid)

        sep2 = QFrame(); sep2.setFrameShape(QFrame.Shape.VLine)
        sep2.setStyleSheet("color:#2a2a3a;"); root.addWidget(sep2)

        # ── Right: apartment list for selected floor ───────────
        right = QVBoxLayout()
        self.apt_floor_label = QLabel("APARTMENTS  (select a floor)")
        right.addWidget(self.apt_floor_label)
        self.apt_table = QTableWidget(0, 3)
        self.apt_table.setHorizontalHeaderLabels(["Apt ID", "LED Start", "LED End"])
        self.apt_table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        self.apt_table.setMinimumWidth(260)
        self.apt_table.cellChanged.connect(self._apt_changed)
        right.addWidget(self.apt_table)
        apt_btns = QHBoxLayout()
        ba_add = QPushButton("+ Apt"); ba_add.clicked.connect(self._add_apt)
        ba_del = QPushButton("- Apt"); ba_del.setObjectName("btn_off"); ba_del.clicked.connect(self._del_apt)
        apt_btns.addWidget(ba_add); apt_btns.addWidget(ba_del)
        right.addLayout(apt_btns)
        root.addLayout(right)

        self._refresh_block_list()
        return w

    def _make_json_tab(self):
        w = QWidget()
        layout = QVBoxLayout(w)
        self.json_edit = QTextEdit()
        self.json_edit.setFont(QFont("Consolas", 10))
        layout.addWidget(QLabel("Raw JSON (changes apply when you click ↑ Apply):"))
        layout.addWidget(self.json_edit)
        btn = QPushButton("↑  Apply JSON to Visual Editor")
        btn.clicked.connect(self._apply_json)
        layout.addWidget(btn)
        self._refresh_json()
        return w

    # ── Helpers ───────────────────────────────────────────────
    def _refresh_json(self):
        self.json_edit.setPlainText(json.dumps(self.data, indent=2, ensure_ascii=False))

    def _apply_json(self):
        try:
            try:
                self.data = json.loads(self.json_edit.toPlainText())
                self._refresh_block_list()
                QMessageBox.information(self, "OK", "JSON applied.")
            except json.JSONDecodeError as e:
                QMessageBox.critical(self, "JSON Error", str(e))
        except Exception as _e:
            import traceback; traceback.print_exc()
    def _refresh_block_list(self):
        try:
            self.block_list.blockSignals(True)
            self.block_list.setRowCount(0)
            for b in self.data.get("blocks", []):
                r = self.block_list.rowCount()
                self.block_list.insertRow(r)
                self.block_list.setItem(r, 0, QTableWidgetItem(str(b["id"])))
                self.block_list.setItem(r, 1, QTableWidgetItem(b.get("slave", "")))
            self.block_list.blockSignals(False)
            self.floor_list.setRowCount(0)
            self.apt_table.setRowCount(0)
            self._refresh_json()
        except Exception as _e:
            import traceback; traceback.print_exc()
    def _current_block_idx(self):
        row = self.block_list.currentRow()
        return row if row >= 0 and self.block_list.selectedItems() else None

    def _current_floor_idx(self):
        row = self.floor_list.currentRow()
        return row if row >= 0 and self.floor_list.selectedItems() else None

    def _on_block_selected(self):
        try:
            idx = self._current_block_idx()
            if idx is None: return
            block = self.data["blocks"][idx]
            self.block_slave_label.setText(f"Block {block['id']}  Slave:")
            self.slave_combo.blockSignals(True)
            self.slave_combo.setCurrentText(block.get("slave", "yellow"))
            self.slave_combo.blockSignals(False)
            self.slave_combo.show()
            self._load_floors(block)
            self.apt_table.setRowCount(0)
            self.apt_floor_label.setText("APARTMENTS  (select a floor)")
        except Exception as _e:
            import traceback; traceback.print_exc()
    def _on_floor_selected(self):
        try:
            bidx = self._current_block_idx()
            fidx = self._current_floor_idx()
            if bidx is None or fidx is None: return
            floor = self.data["blocks"][bidx]["floors"][fidx]
            self.apt_floor_label.setText(f"APARTMENTS  — Floor {floor['floor']}")
            self._load_apts(floor)
        except Exception as _e:
            import traceback; traceback.print_exc()
    def _update_slave(self, val):
        try:
            idx = self._current_block_idx()
            if idx is None: return
            self.data["blocks"][idx]["slave"] = val
            self.block_list.item(idx, 1).setText(val)
            self._refresh_json()
        except Exception as _e:
            import traceback; traceback.print_exc()
    def _load_floors(self, block):
        self.floor_list.blockSignals(True)
        self.floor_list.setRowCount(0)
        for f in block.get("floors", []):
            r = self.floor_list.rowCount()
            self.floor_list.insertRow(r)
            fr = f.get("floorRange", [0, 0])
            self.floor_list.setItem(r, 0, QTableWidgetItem(str(f["floor"])))
            self.floor_list.setItem(r, 1, QTableWidgetItem(str(fr[0])))
            self.floor_list.setItem(r, 2, QTableWidgetItem(str(fr[1])))
        self.floor_list.blockSignals(False)

    def _load_apts(self, floor):
        self.apt_table.blockSignals(True)
        self.apt_table.setRowCount(0)
        for a in floor.get("apartments", []):
            r = self.apt_table.rowCount()
            self.apt_table.insertRow(r)
            ar = _apt_range(a)
            self.apt_table.setItem(r, 0, QTableWidgetItem(str(_apt_id(a))))
            self.apt_table.setItem(r, 1, QTableWidgetItem(str(ar[0])))
            self.apt_table.setItem(r, 2, QTableWidgetItem(str(ar[1])))
        self.apt_table.blockSignals(False)

    def _floor_changed(self, row, col):
        try:
            bidx = self._current_block_idx()
            if bidx is None: return
            floors = self.data["blocks"][bidx].setdefault("floors", [])
            while len(floors) <= row:
                floors.append({"floor": row + 1, "floorRange": [0, 0], "apartments": []})
            try: val = int(self.floor_list.item(row, col).text())
            except (ValueError, AttributeError): return
            if col == 0:   floors[row]["floor"] = val
            elif col == 1: floors[row].setdefault("floorRange", [0, 0])[0] = val
            elif col == 2: floors[row].setdefault("floorRange", [0, 0])[1] = val
            self._refresh_json()
        except Exception as _e:
            import traceback; traceback.print_exc()
    def _apt_changed(self, row, col):
        try:
            bidx = self._current_block_idx()
            fidx = self._current_floor_idx()
            if bidx is None or fidx is None: return
            apts = self.data["blocks"][bidx]["floors"][fidx].setdefault("apartments", [])
            while len(apts) <= row:
                apts.append({"aptID": row + 1, "aptRange": [0, 0]})
            try: val = int(self.apt_table.item(row, col).text())
            except (ValueError, AttributeError): return
            if col == 0:   apts[row]["aptID"] = val  # keep canonical key
            elif col == 1: apts[row].setdefault("aptRange", [0, 0])[0] = val
            elif col == 2: apts[row].setdefault("aptRange", [0, 0])[1] = val
            self._refresh_json()
        except Exception as _e:
            import traceback; traceback.print_exc()
    def _add_block(self):
        try:
            new_id = max((b["id"] for b in self.data.get("blocks", [])), default=0) + 1
            self.data.setdefault("blocks", []).append({"id": new_id, "slave": "yellow", "floors": []})
            self._refresh_block_list()
            self.block_list.selectRow(self.block_list.rowCount() - 1)
        except Exception as _e:
            import traceback; traceback.print_exc()
    def _del_block(self):
        try:
            idx = self._current_block_idx()
            if idx is None: return
            self.data["blocks"].pop(idx)
            self._refresh_block_list()
        except Exception as _e:
            import traceback; traceback.print_exc()
    def _add_floor(self):
        try:
            bidx = self._current_block_idx()
            if bidx is None:
                QMessageBox.warning(self, "No block", "Select a block first"); return
            floors = self.data["blocks"][bidx].setdefault("floors", [])
            next_num = max((f["floor"] for f in floors), default=0) + 1
            last_end = max((f["floorRange"][1] for f in floors), default=-1)
            floors.append({"floor": next_num, "floorRange": [last_end + 1, last_end + 10], "apartments": []})
            self._load_floors(self.data["blocks"][bidx])
            self._refresh_json()
        except Exception as _e:
            import traceback; traceback.print_exc()
    def _del_floor(self):
        try:
            bidx = self._current_block_idx()
            fidx = self._current_floor_idx()
            if bidx is None or fidx is None: return
            self.data["blocks"][bidx]["floors"].pop(fidx)
            self._load_floors(self.data["blocks"][bidx])
            self.apt_table.setRowCount(0)
            self._refresh_json()
        except Exception as _e:
            import traceback; traceback.print_exc()
    def _add_apt(self):
        try:
            bidx = self._current_block_idx()
            fidx = self._current_floor_idx()
            if bidx is None or fidx is None:
                QMessageBox.warning(self, "No floor", "Select a floor first"); return
            floor = self.data["blocks"][bidx]["floors"][fidx]
            apts = floor.setdefault("apartments", [])
            next_id  = max((_apt_id(a) for a in apts), default=0) + 1
            last_end = max((a["aptRange"][1] for a in apts), default=-1)
            apts.append({"aptID": next_id, "aptRange": [last_end + 1, last_end + 5]})
            self._load_apts(floor)
            self._refresh_json()
        except Exception as _e:
            import traceback; traceback.print_exc()
    def _del_apt(self):
        try:
            bidx = self._current_block_idx()
            fidx = self._current_floor_idx()
            if bidx is None or fidx is None: return
            row = self.apt_table.currentRow()
            if row < 0: return
            self.data["blocks"][bidx]["floors"][fidx]["apartments"].pop(row)
            self._load_apts(self.data["blocks"][bidx]["floors"][fidx])
            self._refresh_json()
        except Exception as _e:
            import traceback; traceback.print_exc()
    def _save(self):
        try:
            try: self.data = json.loads(self.json_edit.toPlainText())
            except Exception: pass
            self.accept()
        except Exception as _e:
            import traceback; traceback.print_exc()
    def get_data(self):
        return self.data

# ─── Main Window ──────────────────────────────────────────────
class ESPController(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("EliteHome — ESP32 LED Master Controller")
        self.setMinimumSize(1100, 800)
        self.building = self._load_json()
        self.worker   = SerialWorker()
        self.worker.data_received.connect(self._on_data)
        self.worker.connection_status.connect(self._on_status)
        self._text_buf = ""
        self._build_ui()

    # ── JSON IO ──────────────────────────────────────────────
    def _load_json(self):
        try:
            with open("./building.json", "r", encoding="utf-8") as f:
                return json.load(f)
        except Exception:
            return {"blocks": []}

    def _save_json(self):
        with open("./building.json", "w", encoding="utf-8") as f:
            json.dump(self.building, f, indent=2, ensure_ascii=False)

    # ── Serial callbacks ──────────────────────────────────────
    def _on_status(self, connected):
        if connected:
            self.status_lbl.setText("● CONNECTED")
            self.status_lbl.setObjectName("status_connected")
            self.connect_btn.setText("Disconnect")
        else:
            self.status_lbl.setText("● DISCONNECTED")
            self.status_lbl.setObjectName("status_disconnected")
            self.connect_btn.setText("Connect")
        self.status_lbl.setStyle(self.status_lbl.style())

    def _on_data(self, data):
        self._text_buf += data
        while "\n" in self._text_buf:
            line, self._text_buf = self._text_buf.split("\n", 1)
            if line.strip():
                ts = datetime.now().strftime("%H:%M:%S")
                color = "#ff8888" if "[ERROR]" in line else "#44ff44" if "[TX]" in line else "#88ff88"
                self.monitor.append(f'<span style="color:#555">[{ts}]</span> <span style="color:{color}">{line}</span>')
        self.monitor.verticalScrollBar().setValue(self.monitor.verticalScrollBar().maximum())

    # ── Send helpers ──────────────────────────────────────────
    def _dev(self):
        return self.dev_combo.currentText()

    def _send(self, cmd):
        d = self._dev()
        full = cmd if d == "all" else f"{d} {cmd}"
        self.worker.send(full)

    def _send_raw(self, cmd):
        self.worker.send(cmd)

    # ── UI BUILD ──────────────────────────────────────────────
    def _build_ui(self):
        root = QWidget()
        self.setCentralWidget(root)
        root_layout = QVBoxLayout(root)
        root_layout.setSpacing(6)

        # ── Connection bar ────────────────────────────────────
        conn_group = QGroupBox("CONNECTION")
        conn_row = QHBoxLayout(conn_group)

        conn_row.addWidget(QLabel("Port:"))
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(200)
        conn_row.addWidget(self.port_combo)

        self.refresh_btn = QPushButton("⟳")
        self.refresh_btn.setMaximumWidth(32)
        self.refresh_btn.clicked.connect(self._refresh_ports)
        conn_row.addWidget(self.refresh_btn)

        conn_row.addWidget(QLabel("Baud:"))
        self.baud_combo = QComboBox()
        self.baud_combo.addItems(["115200", "9600", "57600"])
        conn_row.addWidget(self.baud_combo)

        self.connect_btn = QPushButton("Connect")
        self.connect_btn.setObjectName("btn_send")
        self.connect_btn.clicked.connect(self._toggle_connect)
        conn_row.addWidget(self.connect_btn)

        self.status_lbl = QLabel("● DISCONNECTED")
        self.status_lbl.setObjectName("status_disconnected")
        conn_row.addWidget(self.status_lbl)
        conn_row.addStretch()
        root_layout.addWidget(conn_group)

        # ── Main content splitter ─────────────────────────────
        splitter = QSplitter(Qt.Orientation.Horizontal)

        # LEFT PANEL
        left_panel = QWidget()
        left_layout = QVBoxLayout(left_panel)
        left_layout.setSpacing(6)

        tabs = QTabWidget()
        tabs.addTab(self._make_control_tab(),  "Control")
        tabs.addTab(self._make_effects_tab(),  "Effects")
        tabs.addTab(self._make_range_tab(),    "Range")
        tabs.addTab(self._make_building_tab(), "Building")
        left_layout.addWidget(tabs)
        splitter.addWidget(left_panel)

        # RIGHT PANEL — monitor
        right_panel = QWidget()
        right_layout = QVBoxLayout(right_panel)
        right_layout.setSpacing(4)

        monitor_group = QGroupBox("SERIAL MONITOR")
        m_layout = QVBoxLayout(monitor_group)
        self.monitor = QTextEdit()
        self.monitor.setReadOnly(True)
        self.monitor.setFont(QFont("Consolas", 9))
        m_layout.addWidget(self.monitor)

        inp_row = QHBoxLayout()
        self.cmd_input = QLineEdit()
        self.cmd_input.setPlaceholderText("raw command...")
        self.cmd_input.returnPressed.connect(self._send_manual)
        inp_row.addWidget(self.cmd_input)
        send_btn = QPushButton("Send")
        send_btn.setObjectName("btn_send")
        send_btn.clicked.connect(self._send_manual)
        inp_row.addWidget(send_btn)
        clear_btn = QPushButton("Clear")
        clear_btn.clicked.connect(self.monitor.clear)
        inp_row.addWidget(clear_btn)
        m_layout.addLayout(inp_row)

        right_layout.addWidget(monitor_group)
        splitter.addWidget(right_panel)

        splitter.setSizes([580, 460])
        root_layout.addWidget(splitter)

        self._refresh_ports()
        self._update_building_combos()

    # ── Control tab ───────────────────────────────────────────
    def _make_control_tab(self):
        w = QWidget()
        layout = QVBoxLayout(w)

        # Device selector
        dev_group = QGroupBox("TARGET DEVICE")
        dev_row = QHBoxLayout(dev_group)
        dev_row.addWidget(QLabel("Send to:"))
        self.dev_combo = QComboBox()
        self.dev_combo.addItems(DEVICES)
        self.dev_combo.setCurrentText("all")
        dev_row.addWidget(self.dev_combo)
        dev_row.addStretch()
        layout.addWidget(dev_group)

        # Quick commands
        quick_group = QGroupBox("QUICK COMMANDS")
        qgrid = QGridLayout(quick_group)

        btn_on = QPushButton("⬜  ON (white)")
        btn_on.setObjectName("btn_on")
        btn_on.clicked.connect(lambda: self._send("on white"))
        qgrid.addWidget(btn_on, 0, 0)

        btn_off = QPushButton("✕  OFF")
        btn_off.setObjectName("btn_off")
        btn_off.clicked.connect(lambda: self._send("off"))
        qgrid.addWidget(btn_off, 0, 1)

        btn_random = QPushButton("✦  Random")
        btn_random.setObjectName("btn_random")
        btn_random.clicked.connect(lambda: self._send("random"))
        qgrid.addWidget(btn_random, 0, 2)

        btn_solo = QPushButton("◉  Solo")
        btn_solo.clicked.connect(lambda: self._send("solo"))
        btn_solo.setToolTip("Turn on selected device, off all others")
        qgrid.addWidget(btn_solo, 1, 0)

        btn_status = QPushButton("ℹ  Status")
        btn_status.clicked.connect(lambda: self._send("status"))
        qgrid.addWidget(btn_status, 1, 1)

        btn_stop = QPushButton("⏹  Stop Effect")
        btn_stop.clicked.connect(lambda: self._send("off"))
        qgrid.addWidget(btn_stop, 1, 2)

        layout.addWidget(quick_group)

        # Color on
        color_group = QGroupBox("ON WITH COLOR")
        color_row = QHBoxLayout(color_group)
        self.color_combo = QComboBox()
        self.color_combo.addItems(COLORS)
        color_row.addWidget(self.color_combo)
        btn_color_on = QPushButton("Turn On")
        btn_color_on.setObjectName("btn_on")
        btn_color_on.clicked.connect(lambda: self._send(f"on {self.color_combo.currentText()}"))
        color_row.addWidget(btn_color_on)
        btn_fill = QPushButton("Fill (no show)")
        btn_fill.clicked.connect(lambda: self._send(f"fill {self.color_combo.currentText()}"))
        color_row.addWidget(btn_fill)
        btn_show = QPushButton("Show")
        btn_show.clicked.connect(lambda: self._send("show"))
        color_row.addWidget(btn_show)
        layout.addWidget(color_group)

        # Brightness
        bright_group = QGroupBox("BRIGHTNESS")
        bright_row = QHBoxLayout(bright_group)
        self.bright_slider = QSlider(Qt.Orientation.Horizontal)
        self.bright_slider.setRange(0, 255)
        self.bright_slider.setValue(200)
        self.bright_val_lbl = QLabel("200")
        self.bright_slider.valueChanged.connect(lambda v: self.bright_val_lbl.setText(str(v)))
        bright_row.addWidget(self.bright_slider)
        bright_row.addWidget(self.bright_val_lbl)
        btn_bright = QPushButton("Set")
        btn_bright.clicked.connect(lambda: self._send(f"brightness {self.bright_slider.value()}"))
        bright_row.addWidget(btn_bright)
        layout.addWidget(bright_group)

        layout.addStretch()
        return w

    # ── Effects tab ───────────────────────────────────────────
    def _make_effects_tab(self):
        w = QWidget()
        layout = QVBoxLayout(w)

        note = QLabel("Effects run on the selected target device (Control tab)")
        note.setStyleSheet("color: #666688; font-size: 10px; font-style: italic;")
        layout.addWidget(note)

        # blink
        g = QGroupBox("BLINK  —  blink <color> <ms> <times>")
        row = QHBoxLayout(g)
        self.blink_color = QComboBox(); self.blink_color.addItems(COLORS)
        self.blink_ms    = QSpinBox();  self.blink_ms.setRange(10, 5000);  self.blink_ms.setValue(300)
        self.blink_times = QSpinBox();  self.blink_times.setRange(1, 999); self.blink_times.setValue(3)
        row.addWidget(QLabel("Color:")); row.addWidget(self.blink_color)
        row.addWidget(QLabel("ms:"));   row.addWidget(self.blink_ms)
        row.addWidget(QLabel("×:"));    row.addWidget(self.blink_times)
        b = QPushButton("▶"); b.setObjectName("btn_send")
        b.clicked.connect(lambda: self._send(
            f"blink {self.blink_color.currentText()} {self.blink_ms.value()} {self.blink_times.value()}"))
        row.addWidget(b)
        layout.addWidget(g)

        # breath
        g = QGroupBox("BREATH  —  breath <color> <steps> <ms>")
        row = QHBoxLayout(g)
        self.breath_color = QComboBox(); self.breath_color.addItems(COLORS); self.breath_color.setCurrentText("blue")
        self.breath_steps = QSpinBox();  self.breath_steps.setRange(5, 500); self.breath_steps.setValue(50)
        self.breath_ms    = QSpinBox();  self.breath_ms.setRange(1, 1000);   self.breath_ms.setValue(8)
        row.addWidget(QLabel("Color:")); row.addWidget(self.breath_color)
        row.addWidget(QLabel("Steps:")); row.addWidget(self.breath_steps)
        row.addWidget(QLabel("ms:"));   row.addWidget(self.breath_ms)
        b = QPushButton("▶"); b.setObjectName("btn_send")
        b.clicked.connect(lambda: self._send(
            f"breath {self.breath_color.currentText()} {self.breath_steps.value()} {self.breath_ms.value()}"))
        row.addWidget(b)
        layout.addWidget(g)

        # wipe
        g = QGroupBox("WIPE  —  wipe <color> <ms>")
        row = QHBoxLayout(g)
        self.wipe_color = QComboBox(); self.wipe_color.addItems(COLORS); self.wipe_color.setCurrentText("orange")
        self.wipe_ms    = QSpinBox();  self.wipe_ms.setRange(0, 1000); self.wipe_ms.setValue(1)
        row.addWidget(QLabel("Color:")); row.addWidget(self.wipe_color)
        row.addWidget(QLabel("ms:"));   row.addWidget(self.wipe_ms)
        b = QPushButton("▶"); b.setObjectName("btn_send")
        b.clicked.connect(lambda: self._send(
            f"wipe {self.wipe_color.currentText()} {self.wipe_ms.value()}"))
        row.addWidget(b)
        layout.addWidget(g)

        # chase
        g = QGroupBox("CHASE  —  chase <color> <tail> <ms>")
        row = QHBoxLayout(g)
        self.chase_color = QComboBox(); self.chase_color.addItems(COLORS); self.chase_color.setCurrentText("cyan")
        self.chase_tail  = QSpinBox();  self.chase_tail.setRange(1, 500); self.chase_tail.setValue(30)
        self.chase_ms    = QSpinBox();  self.chase_ms.setRange(0, 1000);  self.chase_ms.setValue(2)
        row.addWidget(QLabel("Color:")); row.addWidget(self.chase_color)
        row.addWidget(QLabel("Tail:"));  row.addWidget(self.chase_tail)
        row.addWidget(QLabel("ms:"));    row.addWidget(self.chase_ms)
        b = QPushButton("▶"); b.setObjectName("btn_send")
        b.clicked.connect(lambda: self._send(
            f"chase {self.chase_color.currentText()} {self.chase_tail.value()} {self.chase_ms.value()}"))
        row.addWidget(b)
        layout.addWidget(g)

        # sparkle
        g = QGroupBox("SPARKLE  —  sparkle <color> <count> <ms> <iter>")
        row = QHBoxLayout(g)
        self.spark_color = QComboBox(); self.spark_color.addItems(COLORS)
        self.spark_count = QSpinBox();  self.spark_count.setRange(1, 2000); self.spark_count.setValue(200)
        self.spark_ms    = QSpinBox();  self.spark_ms.setRange(10, 2000);   self.spark_ms.setValue(80)
        self.spark_iter  = QSpinBox();  self.spark_iter.setRange(1, 9999);  self.spark_iter.setValue(0)
        self.spark_iter.setToolTip("0 = loop forever")
        row.addWidget(QLabel("Color:")); row.addWidget(self.spark_color)
        row.addWidget(QLabel("Count:")); row.addWidget(self.spark_count)
        row.addWidget(QLabel("ms:"));   row.addWidget(self.spark_ms)
        row.addWidget(QLabel("iter:"));  row.addWidget(self.spark_iter)
        b = QPushButton("▶"); b.setObjectName("btn_send")
        b.clicked.connect(lambda: self._send(
            f"sparkle {self.spark_color.currentText()} {self.spark_count.value()} {self.spark_ms.value()} {self.spark_iter.value()}"))
        row.addWidget(b)
        layout.addWidget(g)

        # strobe
        g = QGroupBox("STROBE  —  strobe <color> <onMs> <offMs> <times>")
        row = QHBoxLayout(g)
        self.strobe_color = QComboBox(); self.strobe_color.addItems(COLORS)
        self.strobe_on    = QSpinBox();  self.strobe_on.setRange(10, 2000);  self.strobe_on.setValue(50)
        self.strobe_off   = QSpinBox();  self.strobe_off.setRange(10, 2000); self.strobe_off.setValue(50)
        self.strobe_times = QSpinBox();  self.strobe_times.setRange(1, 999); self.strobe_times.setValue(10)
        row.addWidget(QLabel("Color:"));  row.addWidget(self.strobe_color)
        row.addWidget(QLabel("On ms:"));  row.addWidget(self.strobe_on)
        row.addWidget(QLabel("Off ms:")); row.addWidget(self.strobe_off)
        row.addWidget(QLabel("×:"));      row.addWidget(self.strobe_times)
        b = QPushButton("▶"); b.setObjectName("btn_send")
        b.clicked.connect(lambda: self._send(
            f"strobe {self.strobe_color.currentText()} {self.strobe_on.value()} {self.strobe_off.value()} {self.strobe_times.value()}"))
        row.addWidget(b)
        layout.addWidget(g)

        layout.addStretch()
        return w

    # ── Range tab ─────────────────────────────────────────────
    def _make_range_tab(self):
        w = QWidget()
        layout = QVBoxLayout(w)

        # Basic range
        basic_group = QGroupBox("RANGE  —  range <start> <end> [color]")
        bgrid = QGridLayout(basic_group)

        bgrid.addWidget(QLabel("Start:"), 0, 0)
        self.r_start = QSpinBox(); self.r_start.setRange(0, 99999); self.r_start.setValue(0)
        bgrid.addWidget(self.r_start, 0, 1)

        s_btns = QHBoxLayout()
        for d in (-10, -1, +1, +10):
            b = QPushButton(f"{'+' if d>0 else ''}{d}")
            b.setMaximumWidth(46)
            def _adj_start(_, dv=d):
                self.r_start.setValue(max(0, self.r_start.value() + dv))
                self._send_range()
            b.clicked.connect(_adj_start)
            s_btns.addWidget(b)
        bgrid.addLayout(s_btns, 0, 2)

        bgrid.addWidget(QLabel("End:"), 1, 0)
        self.r_end = QSpinBox(); self.r_end.setRange(0, 99999); self.r_end.setValue(100)
        bgrid.addWidget(self.r_end, 1, 1)

        e_btns = QHBoxLayout()
        for d in (-10, -1, +1, +10):
            b = QPushButton(f"{'+' if d>0 else ''}{d}")
            b.setMaximumWidth(46)
            def _adj_end(_, dv=d):
                self.r_end.setValue(max(0, self.r_end.value() + dv))
                self._send_range()
            b.clicked.connect(_adj_end)
            e_btns.addWidget(b)
        bgrid.addLayout(e_btns, 1, 2)

        bgrid.addWidget(QLabel("Color:"), 2, 0)
        self.r_color = QComboBox(); self.r_color.addItems(COLORS)
        bgrid.addWidget(self.r_color, 2, 1)

        nav_row = QHBoxLayout()
        btn_prev = QPushButton("◀ Prev range")
        btn_prev.clicked.connect(self._range_prev)
        nav_row.addWidget(btn_prev)
        btn_send_range = QPushButton("▶ Send range")
        btn_send_range.setObjectName("btn_send")
        btn_send_range.clicked.connect(self._send_range)
        nav_row.addWidget(btn_send_range)
        btn_next = QPushButton("Next range ▶")
        btn_next.clicked.connect(self._range_next)
        nav_row.addWidget(btn_next)
        bgrid.addLayout(nav_row, 3, 0, 1, 3)

        layout.addWidget(basic_group)

        # Range effect (reffect)
        reff_group = QGroupBox("REFFECT  —  Apply effect to range only")
        rgrid = QGridLayout(reff_group)

        rgrid.addWidget(QLabel("Start:"), 0, 0)
        self.re_start = QSpinBox(); self.re_start.setRange(0, 99999); self.re_start.setValue(0)
        rgrid.addWidget(self.re_start, 0, 1)

        rgrid.addWidget(QLabel("End:"), 0, 2)
        self.re_end = QSpinBox(); self.re_end.setRange(0, 99999); self.re_end.setValue(500)
        rgrid.addWidget(self.re_end, 0, 3)

        rgrid.addWidget(QLabel("Effect:"), 1, 0)
        self.re_effect = QComboBox(); self.re_effect.addItems(EFFECTS + ["fill", "off"])
        self.re_effect.currentTextChanged.connect(self._update_reffect_params)
        rgrid.addWidget(self.re_effect, 1, 1)

        rgrid.addWidget(QLabel("Color:"), 1, 2)
        self.re_color = QComboBox(); self.re_color.addItems(COLORS)
        rgrid.addWidget(self.re_color, 1, 3)

        # Dynamic params row
        self.re_params_widget = QWidget()
        self.re_params_row = QHBoxLayout(self.re_params_widget)
        self.re_params_row.setContentsMargins(0,0,0,0)
        rgrid.addWidget(self.re_params_widget, 2, 0, 1, 4)

        btn_reff = QPushButton("▶ Send reffect")
        btn_reff.setObjectName("btn_send")
        btn_reff.clicked.connect(self._send_reffect)
        rgrid.addWidget(btn_reff, 3, 0, 1, 4)

        layout.addWidget(reff_group)
        layout.addStretch()

        self._update_reffect_params()
        return w

    def _update_reffect_params(self):
        # Clear old param widgets safely
        while self.re_params_row.count():
            item = self.re_params_row.takeAt(0)
            w = item.widget() if item else None
            if w is not None:
                w.setParent(None)  # safer than deleteLater() in slot context

        effect = self.re_effect.currentText()
        self.re_spins = {}

        def add_spin(label, key, lo, hi, val):
            self.re_params_row.addWidget(QLabel(f"{label}:"))
            sp = QSpinBox(); sp.setRange(lo, hi); sp.setValue(val)
            sp.setMaximumWidth(80)
            self.re_spins[key] = sp
            self.re_params_row.addWidget(sp)

        if effect == "blink":
            add_spin("ms",    "ms",    10, 5000, 300)
            add_spin("times", "times",  1,  999,   3)
        elif effect == "breath":
            add_spin("steps", "steps",  5,  500, 50)
            add_spin("ms",    "ms",     1, 1000,  8)
        elif effect == "wipe":
            add_spin("ms", "ms", 0, 1000, 1)
        elif effect == "chase":
            add_spin("tail", "tail", 1, 500, 30)
            add_spin("ms",   "ms",   0, 500,  2)
        elif effect == "sparkle":
            add_spin("count", "count",  1, 2000, 100)
            add_spin("ms",    "ms",    10, 2000,  80)
            add_spin("iter",  "iter",   1, 9999,  20)
        elif effect == "strobe":
            add_spin("onMs",  "onMs",  10, 2000, 50)
            add_spin("offMs", "offMs", 10, 2000, 50)
            add_spin("times", "times",  1,  999, 10)

        self.re_params_row.addStretch()

    def _send_reffect(self):
        rs = self.re_start.value()
        re = self.re_end.value()
        effect = self.re_effect.currentText()
        color  = self.re_color.currentText()

        if effect == "fill":
            self._send(f"reffect {rs} {re} fill {color}")
            return
        if effect == "off":
            self._send(f"reffect {rs} {re} off")
            return

        sp = self.re_spins
        if effect == "blink":
            cmd = f"reffect {rs} {re} blink {color} {sp['ms'].value()} {sp['times'].value()}"
        elif effect == "breath":
            cmd = f"reffect {rs} {re} breath {color} {sp['steps'].value()} {sp['ms'].value()}"
        elif effect == "wipe":
            cmd = f"reffect {rs} {re} wipe {color} {sp['ms'].value()}"
        elif effect == "chase":
            cmd = f"reffect {rs} {re} chase {color} {sp['tail'].value()} {sp['ms'].value()}"
        elif effect == "sparkle":
            cmd = f"reffect {rs} {re} sparkle {color} {sp['count'].value()} {sp['ms'].value()} {sp['iter'].value()}"
        elif effect == "strobe":
            cmd = f"reffect {rs} {re} strobe {color} {sp['onMs'].value()} {sp['offMs'].value()} {sp['times'].value()}"
        else:
            return
        self._send(cmd)

    def _send_range(self):
        s = self.r_start.value()
        e = self.r_end.value()
        color = self.r_color.currentText()
        if color == "white":
            self._send(f"range {s} {e}")
        else:
            self._send(f"range {s} {e} {color}")

    def _range_prev(self):
        s, e = self.r_start.value(), self.r_end.value()
        size = e - s
        new_e = max(size, s - 1)
        new_s = new_e - size
        self.r_start.setValue(new_s)
        self.r_end.setValue(new_e)
        self._send_range()

    def _range_next(self):
        s, e = self.r_start.value(), self.r_end.value()
        size = e - s
        new_s = e + 1
        new_e = new_s + size
        self.r_start.setValue(new_s)
        self.r_end.setValue(new_e)
        self._send_range()

    # ── Building tab ──────────────────────────────────────────
    def _make_building_tab(self):
        w = QWidget()
        layout = QVBoxLayout(w)

        # Selectors
        sel_group = QGroupBox("SELECT")
        sgrid = QGridLayout(sel_group)

        sgrid.addWidget(QLabel("Block:"), 0, 0)
        self.b_block = QComboBox()
        self.b_block.currentIndexChanged.connect(self._update_floors_combo)
        sgrid.addWidget(self.b_block, 0, 1)

        sgrid.addWidget(QLabel("Floor:"), 0, 2)
        self.b_floor = QComboBox()
        self.b_floor.currentIndexChanged.connect(self._update_apts_combo)
        sgrid.addWidget(self.b_floor, 0, 3)

        sgrid.addWidget(QLabel("Apartment:"), 1, 0)
        self.b_apt = QComboBox()
        sgrid.addWidget(self.b_apt, 1, 1)

        layout.addWidget(sel_group)

        # Action buttons
        act_group = QGroupBox("ILLUMINATE")
        agrid = QGridLayout(act_group)

        btn_apt = QPushButton("💡 Apartment")
        btn_apt.setObjectName("btn_on")
        btn_apt.clicked.connect(self._send_apt)
        agrid.addWidget(btn_apt, 0, 0)

        btn_floor = QPushButton("🏢 Floor")
        btn_floor.setObjectName("btn_on")
        btn_floor.clicked.connect(self._send_floor)
        agrid.addWidget(btn_floor, 0, 1)

        btn_block = QPushButton("🏗️ Block")
        btn_block.setObjectName("btn_on")
        btn_block.clicked.connect(self._send_block)
        agrid.addWidget(btn_block, 0, 2)

        btn_off_all = QPushButton("✕ Off All")
        btn_off_all.setObjectName("btn_off")
        btn_off_all.clicked.connect(lambda: self._send_raw("off"))
        agrid.addWidget(btn_off_all, 0, 3)

        layout.addWidget(act_group)

        # Edit config button
        edit_btn = QPushButton("⚙️  Edit Building Configuration (Floors & Apartments)")
        edit_btn.setObjectName("btn_send")
        edit_btn.clicked.connect(self._open_editor)
        layout.addWidget(edit_btn)

        layout.addStretch()
        return w

    def _update_building_combos(self):
        self.b_block.blockSignals(True)
        self.b_block.clear()
        for b in self.building.get("blocks", []):
            self.b_block.addItem(f"Block {b['id']}  [{b.get('slave','')}]", userData=b)
        self.b_block.blockSignals(False)
        self._update_floors_combo()

    def _update_floors_combo(self):
        self.b_floor.blockSignals(True)
        self.b_floor.clear()
        bidx = self.b_block.currentIndex()
        blocks = self.building.get("blocks", [])
        if 0 <= bidx < len(blocks):
            for f in blocks[bidx].get("floors", []):
                fr = f.get("floorRange", [0, 0])
                self.b_floor.addItem(f"Floor {f['floor']}  [{fr[0]}–{fr[1]}]", userData=f)
        self.b_floor.blockSignals(False)
        self._update_apts_combo()

    def _update_apts_combo(self):
        self.b_apt.clear()
        floor_data = self.b_floor.currentData()
        if not floor_data: return
        for a in floor_data.get("apartments", []):
            ar = _apt_range(a)
            self.b_apt.addItem(f"Apt {_apt_id(a)}  [{ar[0]}–{ar[1]}]", userData=a)

    def _get_block(self):
        idx = self.b_block.currentIndex()
        blocks = self.building.get("blocks", [])
        return blocks[idx] if 0 <= idx < len(blocks) else None

    def _send_apt(self):
        block = self._get_block()
        if not block: return
        apt_data = self.b_apt.currentData()
        if not apt_data:
            QMessageBox.warning(self, "No Apartment", "Select an apartment"); return
        ar = _apt_range(apt_data)
        slave = block.get("slave", "all")
        cmd = f"range {ar[0]} {ar[1]}" if slave == "all" else f"{slave} range {ar[0]} {ar[1]}"
        self._send_raw(cmd)

    def _send_floor(self):
        block = self._get_block()
        if not block: return
        floor_data = self.b_floor.currentData()
        if not floor_data:
            QMessageBox.warning(self, "No Floor", "Select a floor"); return
        fr = floor_data.get("floorRange", [0, 0])
        slave = block.get("slave", "all")
        cmd = f"range {fr[0]} {fr[1]}" if slave == "all" else f"{slave} range {fr[0]} {fr[1]}"
        self._send_raw(cmd)

    def _send_block(self):
        block = self._get_block()
        if not block: return
        floors = block.get("floors", [])
        if not floors:
            QMessageBox.warning(self, "Empty", "Block has no floors"); return
        start = min(f["floorRange"][0] for f in floors)
        end   = max(f["floorRange"][1] for f in floors)
        slave = block.get("slave", "all")
        cmd = f"range {start} {end}" if slave == "all" else f"{slave} range {start} {end}"
        self._send_raw(cmd)

    def _open_editor(self):
        dlg = BuildingEditorDialog(self.building, self)
        if dlg.exec():
            self.building = dlg.get_data()
            try:
                self._save_json()
            except Exception as e:
                QMessageBox.warning(self, "Save Error", str(e))
            self._update_building_combos()

    # ── Connection ────────────────────────────────────────────
    def _refresh_ports(self):
        self.port_combo.clear()
        for p in serial.tools.list_ports.comports():
            self.port_combo.addItem(f"{p.device}  —  {p.description}")

    def _toggle_connect(self):
        if self.connect_btn.text() == "Connect":
            txt = self.port_combo.currentText()
            if not txt:
                QMessageBox.warning(self, "No port", "Select a serial port"); return
            port = txt.split()[0]
            baud = int(self.baud_combo.currentText())
            self.worker.connect(port, baud)
        else:
            self.worker.disconnect()

    def _send_manual(self):
        cmd = self.cmd_input.text().strip()
        if cmd:
            self._send_raw(cmd)
            self.cmd_input.clear()

    def closeEvent(self, event):
        try: self.worker.disconnect()
        except Exception: pass
        event.accept()


# ─── Entry point ──────────────────────────────────────────────
if __name__ == "__main__":
    app = QApplication(sys.argv)
    app.setStyleSheet(DARK_STYLE)
    win = ESPController()
    win.show()
    sys.exit(app.exec())