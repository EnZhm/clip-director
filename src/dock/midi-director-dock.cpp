/*
Clip Director: MIDI Director dock (W4). Maps a MIDI control to any OBS hotkey, so the dock stays
decoupled from the plugins it drives: bind a pad to cd_skip, pd_drop_now, cd_syn_bang - whatever the
scene collection has - by identity (hotkey name + registerer).

Layout: device picker + activity light, Learn (pick a hotkey from the searchable list, press Learn,
move a control), the mapping table (control | target | mode, remove per row) and the last-message
monitor. Mappings are saved with the scene collection under the "midi_director" key.

The WinMM callback only queues bytes; this dock's ~10 ms timer drains that queue on the UI thread,
frames it into MIDI messages and triggers hotkeys from here.
*/
#include "midi-director-dock.h"

#if defined(ENABLE_FRONTEND_API) && defined(ENABLE_QT)

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QComboBox>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdint>
#include <string>
#include <vector>

#include "core/midi_map.h"
#include "obs/cd-hotkeys.h"
#include "obs/cd-midi-input.h"

namespace {

const char *kDockId = "midi-director-dock";
const char *kSaveKey = "midi_director";
const char *kLightOn = "border-radius:6px; background:#2f9e44;";
const char *kLightOff = "border-radius:6px; background:#343a40;";

constexpr int kPollMs = 10;        // drain the MIDI queue this often
constexpr int kActivityTicks = 15; // the light stays lit for 150 ms
constexpr int kHotkeyRefreshMs = 3000;

QString text_of(const std::string &s)
{
	return QString::fromStdString(s);
}

QString target_label(const cd::midi::Target &t)
{
	QString label = text_of(t.description);
	if (!t.registerer_name.empty())
		label += QString(" · %1").arg(text_of(t.registerer_name));
	return label;
}

QString target_tooltip(const cd::midi::Target &t)
{
	QString tip = QString("%1 (%2").arg(text_of(t.name)).arg(text_of(cd::midi::registerer_to_string(t.registerer)));
	if (!t.registerer_name.empty())
		tip += QString(" %1").arg(text_of(t.registerer_name));
	return tip + ")";
}

class MidiDirectorDock : public QWidget {
public:
	MidiDirectorDock() : QWidget(nullptr)
	{
		setObjectName("MidiDirectorDock");
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(6, 6, 6, 6);
		root->setSpacing(6);

		/* device picker + activity light */
		auto *top = new QHBoxLayout();
		devices_ = new QComboBox();
		devices_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		devices_->setMinimumContentsLength(12);
		top->addWidget(devices_, 1);
		refresh_ = new QPushButton(obs_module_text("MidiRefresh"));
		top->addWidget(refresh_);
		activity_ = new QLabel();
		activity_->setFixedSize(12, 12);
		activity_->setStyleSheet(kLightOff);
		top->addWidget(activity_);
		root->addLayout(top);

		status_ = new QLabel();
		status_->setWordWrap(true);
		status_->setStyleSheet("color:#999;");
		root->addWidget(status_);

		/* learn: searchable hotkey list, the mode the new binding gets, and the Learn button */
		auto *learn_row = new QHBoxLayout();
		search_ = new QLineEdit();
		search_->setPlaceholderText(obs_module_text("MidiLearn.Search"));
		learn_row->addWidget(search_, 1);
		mode_ = new QComboBox();
		mode_->addItem(obs_module_text("MidiMode.Momentary"), (int)cd::midi::Mode::Momentary);
		mode_->addItem(obs_module_text("MidiMode.Toggle"), (int)cd::midi::Mode::Toggle);
		mode_->setToolTip(obs_module_text("MidiMode"));
		learn_row->addWidget(mode_);
		learn_button_ = new QPushButton(obs_module_text("MidiLearn"));
		learn_row->addWidget(learn_button_);
		root->addLayout(learn_row);

		hotkeys_ = new QListWidget();
		hotkeys_->setMinimumHeight(90);
		hotkeys_->setToolTip(obs_module_text("MidiLearn"));
		root->addWidget(hotkeys_);

		/* mapping table */
		head_ = new QLabel(obs_module_text("MidiMappings"));
		head_->setStyleSheet("font-weight:bold;");
		root->addWidget(head_);
		table_ = new QTableWidget(0, 4);
		table_->setHorizontalHeaderLabels({obs_module_text("MidiColumn.Control"),
						   obs_module_text("MidiColumn.Target"),
						   obs_module_text("MidiColumn.Mode"), ""});
		table_->verticalHeader()->hide();
		table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
		table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
		table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
		table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
		table_->setSelectionMode(QAbstractItemView::NoSelection);
		table_->setMinimumHeight(110);
		root->addWidget(table_, 1);

		/* last-message monitor */
		monitor_head_ = new QLabel(obs_module_text("MidiMonitor"));
		monitor_head_->setStyleSheet("font-weight:bold;");
		root->addWidget(monitor_head_);
		last_ = new QLabel("-");
		last_->setStyleSheet("font-family:Consolas,monospace; font-size:11px;");
		root->addWidget(last_);
		monitor_ = new QPlainTextEdit();
		monitor_->setReadOnly(true);
		monitor_->setMaximumBlockCount(200);
		monitor_->setLineWrapMode(QPlainTextEdit::NoWrap);
		monitor_->setStyleSheet("font-family:Consolas,monospace; font-size:11px;");
		monitor_->setMinimumHeight(60);
		root->addWidget(monitor_, 1);

		connect(refresh_, &QPushButton::clicked, this, [this] { list_devices(); });
		connect(devices_, &QComboBox::activated, this, [this](int) { select_device(); });
		connect(search_, &QLineEdit::textChanged, this, [this](const QString &) { refresh_hotkeys(false); });
		connect(learn_button_, &QPushButton::clicked, this, [this] { toggle_learn(); });

		auto *fast = new QTimer(this);
		fast->setInterval(kPollMs);
		connect(fast, &QTimer::timeout, this, [this] { poll(); });
		fast->start();
		auto *slow = new QTimer(this);
		slow->setInterval(kHotkeyRefreshMs);
		connect(slow, &QTimer::timeout, this, [this] { refresh_hotkeys(false); });
		slow->start();

		list_devices();
		refresh_hotkeys(true);
		rebuild_table();
		obs_frontend_add_save_callback(save_callback, this);
		show_status(cdmidi::available() ? no_device() : QString(obs_module_text("MidiStatus.NoBackend")));
	}

	~MidiDirectorDock() override
	{
		obs_frontend_remove_save_callback(save_callback, this);
		input_.close();
	}

private:
	static QString no_device() { return QString(obs_module_text("MidiStatus.Closed")); }

	/* ----------------------------------------------------------------------------------- devices */

	void list_devices()
	{
		const std::vector<cdmidi::Device> found = cdmidi::devices();
		QStringList names;
		names << obs_module_text("MidiDevice.None");
		for (const auto &d : found)
			names << text_of(d.name);
		QStringList had;
		for (int i = 0; i < devices_->count(); ++i)
			had << devices_->itemText(i);
		if (names == had)
			return;

		devices_->blockSignals(true);
		devices_->clear();
		devices_->addItem(obs_module_text("MidiDevice.None"), -1);
		for (const auto &d : found)
			devices_->addItem(text_of(d.name), d.index);
		int select = 0;
		if (input_.is_open()) {
			const int at = devices_->findData(input_.device_index());
			if (at >= 0) {
				select = at;
			} else {
				input_.close(); /* unplugged while open */
				assembler_.reset();
				map_.reset_runtime();
				show_status(no_device());
			}
		}
		devices_->setCurrentIndex(select);
		devices_->blockSignals(false);
	}

	void select_device()
	{
		const int index = devices_->currentData().toInt();
		if (index < 0) {
			input_.close();
			assembler_.reset();
			map_.reset_runtime();
			show_status(no_device());
			return;
		}
		std::string error;
		if (!input_.open(index, &error)) {
			show_status(QString(obs_module_text("MidiStatus.OpenError")).arg(text_of(error)), true);
			return;
		}
		assembler_.reset();
		map_.reset_runtime();
		show_status(QString(obs_module_text("MidiStatus.Opened")).arg(devices_->currentText()));
		obs_log(LOG_INFO, "midi director: listening on device %d", index);
	}

	/* ------------------------------------------------------------------------------------ learn */

	void refresh_hotkeys(bool force)
	{
		const QString filter = search_->text().trimmed();
		std::vector<cdhotkey::Entry> shown;
		for (const auto &entry : cdhotkey::list()) {
			const bool hit = filter.isEmpty() ||
					 text_of(entry.description).contains(filter, Qt::CaseInsensitive) ||
					 text_of(entry.name).contains(filter, Qt::CaseInsensitive) ||
					 text_of(entry.registerer_name).contains(filter, Qt::CaseInsensitive);
			if (hit)
				shown.push_back(entry);
		}

		QStringList keys;
		for (const auto &entry : shown)
			keys << QString("%1|%2|%3|%4")
					.arg(text_of(entry.name))
					.arg(text_of(entry.registerer))
					.arg(text_of(entry.registerer_name))
					.arg(text_of(entry.description));
		if (!force && keys == hotkey_keys_)
			return;
		const QString keep = hotkeys_->currentItem() ? hotkeys_->currentItem()->text() : QString();
		hotkey_keys_ = keys;
		targets_ = shown;

		hotkeys_->blockSignals(true);
		hotkeys_->clear();
		for (size_t i = 0; i < targets_.size(); ++i) {
			auto *item = new QListWidgetItem(target_label(midi_target(targets_[i])));
			item->setToolTip(target_tooltip(midi_target(targets_[i])));
			item->setData(Qt::UserRole, (int)i);
			hotkeys_->addItem(item);
			if (!keep.isEmpty() && item->text() == keep)
				hotkeys_->setCurrentItem(item);
		}
		hotkeys_->blockSignals(false);
	}

	void toggle_learn()
	{
		if (learn_.armed()) {
			learn_.cancel();
			learn_button_->setText(obs_module_text("MidiLearn"));
			show_status(QString(obs_module_text("MidiStatus.Cancelled")));
			return;
		}
		auto *item = hotkeys_->currentItem();
		const int row = item ? item->data(Qt::UserRole).toInt() : -1;
		if (row < 0 || row >= (int)targets_.size()) {
			show_status(QString(obs_module_text("MidiHotkey.None")), true);
			return;
		}
		const auto mode = (cd::midi::Mode)mode_->currentData().toInt();
		learn_.arm(midi_target(targets_[row]), mode);
		learn_button_->setText(obs_module_text("MidiLearn.Cancel"));
		show_status(QString(obs_module_text("MidiWaiting")).arg(item->text()));
	}

	/* ------------------------------------------------------------------------------------- poll */

	void poll()
	{
		std::vector<uint8_t> bytes;
		input_.drain(bytes);
		if (!bytes.empty()) {
			activity_->setStyleSheet(kLightOn);
			activity_ticks_ = kActivityTicks;
			std::vector<cd::midi::Message> messages;
			assembler_.feed(bytes.data(), bytes.size(), messages);
			for (const auto &m : messages)
				on_message(m);
		}
		if (activity_ticks_ > 0 && --activity_ticks_ == 0)
			activity_->setStyleSheet(kLightOff);
		flush_monitor();
	}

	void on_message(const cd::midi::Message &m)
	{
		++messages_;
		pending_ = describe(m);
		++pending_count_;

		if (learn_.armed()) {
			const cd::midi::Control control = cd::midi::Control::of(m);
			if (learn_.feed(m, map_) != nullptr) {
				learn_button_->setText(obs_module_text("MidiLearn"));
				rebuild_table();
				show_status(QString(obs_module_text("MidiStatus.Bound")).arg(control_label(control)));
			}
			return;
		}
		for (const cd::midi::Event &event : map_.handle(m))
			cdhotkey::trigger(hotkey_entry(event.target), event.pressed);
	}

	void flush_monitor()
	{
		if (pending_count_ == 0)
			return;
		QString line = pending_;
		if (pending_count_ > 1)
			line += QString("  (+%1)").arg(pending_count_ - 1);
		pending_count_ = 0;
		last_->setText(pending_);
		monitor_->appendPlainText(line);
		monitor_head_->setText(QString("%1 (%2)").arg(obs_module_text("MidiMonitor")).arg(messages_));
	}

	/* ------------------------------------------------------------------------------ mapping table */

	void rebuild_table()
	{
		const std::vector<cd::midi::Mapping> rows = map_.mappings();
		table_->setRowCount((int)rows.size());
		for (int r = 0; r < (int)rows.size(); ++r) {
			const cd::midi::Mapping &m = rows[r];
			auto *control = new QTableWidgetItem(control_label(m.control));
			control->setFlags(Qt::ItemIsEnabled);
			auto *target = new QTableWidgetItem(target_label(m.target));
			target->setToolTip(target_tooltip(m.target));
			target->setFlags(Qt::ItemIsEnabled);

			auto *mode = new QComboBox();
			mode->addItem(obs_module_text("MidiMode.Momentary"), (int)cd::midi::Mode::Momentary);
			mode->addItem(obs_module_text("MidiMode.Toggle"), (int)cd::midi::Mode::Toggle);
			mode->setCurrentIndex(m.mode == cd::midi::Mode::Toggle ? 1 : 0);
			const cd::midi::Control control_key = m.control;
			connect(mode, &QComboBox::activated, this, [this, control_key, mode](int) {
				const cd::midi::Mapping *bound = map_.find(control_key);
				if (bound)
					map_.bind(control_key, bound->target,
						  (cd::midi::Mode)mode->currentData().toInt());
			});

			auto *remove = new QPushButton(obs_module_text("MidiRemove"));
			connect(remove, &QPushButton::clicked, this, [this, control_key] {
				map_.unbind(control_key);
				/* the clicked button lives in the row we are about to delete: rebuild
				 * once its signal has been delivered */
				QTimer::singleShot(0, this, [this] { rebuild_table(); });
			});

			table_->setItem(r, 0, control);
			table_->setItem(r, 1, target);
			table_->setCellWidget(r, 2, mode);
			table_->setCellWidget(r, 3, remove);
		}
	}

	/* ---------------------------------------------------------------------------------- helpers */

	void show_status(const QString &text, bool error = false)
	{
		status_->setText(text);
		status_->setStyleSheet(error ? "color:#e03131;" : "color:#999;");
		if (error)
			obs_log(LOG_WARNING, "midi director: %s", text.toUtf8().constData());
	}

	static QString control_label(const cd::midi::Control &c) { return text_of(c.label()); }

	static QString describe(const cd::midi::Message &m)
	{
		return QString("%1 %2 ch %3 %4  v%5")
			.arg(m.type == cd::midi::Type::Control ? "CC" : "Note")
			.arg(m.number)
			.arg(m.channel)
			.arg(m.pressed ? "on" : "off")
			.arg(m.value);
	}

	static cd::midi::Target midi_target(const cdhotkey::Entry &entry)
	{
		cd::midi::Target target;
		target.name = entry.name;
		cd::midi::Registerer registerer = cd::midi::Registerer::Frontend;
		cd::midi::registerer_from_string(entry.registerer, registerer);
		target.registerer = registerer;
		target.registerer_name = entry.registerer_name;
		target.description = entry.description;
		return target;
	}

	static cdhotkey::Entry hotkey_entry(const cd::midi::Target &target)
	{
		cdhotkey::Entry entry;
		entry.name = target.name;
		entry.registerer = cd::midi::registerer_to_string(target.registerer);
		entry.registerer_name = target.registerer_name;
		entry.description = target.description;
		return entry;
	}

	/* ------------------------------------------------------------------------------- persistence */

	// OBS calls save callbacks with saving=false after a scene collection loads (and on every
	// collection switch), so the mappings come back from the collection the user is on. There is
	// no public getter for the loaded collection's data, so this is the only entry point.
	static void save_callback(obs_data_t *save_data, bool saving, void *data)
	{
		auto *dock = static_cast<MidiDirectorDock *>(data);
		if (!dock || !save_data)
			return;
		if (saving) {
			const std::string json = dock->map_.to_json();
			obs_data_set_string(save_data, kSaveKey, json.c_str());
			return;
		}
		const char *json = obs_data_get_string(save_data, kSaveKey);
		if (!json || !*json)
			return;
		std::string error;
		if (!dock->map_.from_json(json, &error)) {
			dock->show_status(QString(obs_module_text("MidiStatus.Dropped")).arg(text_of(error)), true);
			return;
		}
		dock->rebuild_table();
		obs_log(LOG_INFO, "midi director: restored %d mappings", (int)dock->map_.size());
	}

private:
	QComboBox *devices_;
	QPushButton *refresh_;
	QLabel *activity_, *status_, *last_, *head_, *monitor_head_;
	QLineEdit *search_;
	QComboBox *mode_;
	QPushButton *learn_button_;
	QListWidget *hotkeys_;
	QTableWidget *table_;
	QPlainTextEdit *monitor_;

	cdmidi::Input input_;
	cd::midi::Assembler assembler_;
	cd::midi::Map map_;
	cd::midi::Learn learn_;
	std::vector<cdhotkey::Entry> targets_;
	QStringList hotkey_keys_;
	QString pending_;
	int pending_count_ = 0;
	int activity_ticks_ = 0;
	uint64_t messages_ = 0;
};

MidiDirectorDock *g_dock = nullptr;

} // namespace

extern "C" void clip_director_midi_dock_load(void)
{
	auto *dock = new MidiDirectorDock();
	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("MidiDirector"), dock)) {
		obs_log(LOG_ERROR, "dock id already in use; MIDI Director not added");
		delete dock;
		return;
	}
	g_dock = dock;
	obs_log(LOG_INFO, "MIDI Director dock registered");

#if defined(CD_TEST_HOOKS)
	/* sandbox only (-DCD_TEST_HOOKS=ON, never in releases): CD_MIDI_DOCK_SNAPSHOT=<png path> opens
	 * the dock docked right and saves a grab of it every 2 s */
	const QString snap = qEnvironmentVariable("CD_MIDI_DOCK_SNAPSHOT");
	if (!snap.isEmpty()) {
		obs_log(LOG_INFO, "test hooks: MIDI dock snapshots to %s", snap.toUtf8().constData());
		QTimer::singleShot(8000, dock, [dock] {
			auto *dw = qobject_cast<QDockWidget *>(dock->parentWidget());
			auto *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window());
			if (!dw || !mw)
				return;
			dw->setFloating(false);
			mw->addDockWidget(Qt::RightDockWidgetArea, dw);
			dw->show();
			dw->raise();
			mw->resizeDocks({dw}, {420}, Qt::Horizontal);
		});
		auto *t = new QTimer(dock);
		t->setInterval(2000);
		QObject::connect(t, &QTimer::timeout, dock, [dock, snap] { dock->grab().save(snap); });
		t->start();
	}
#endif
}

extern "C" void clip_director_midi_dock_unload(void)
{
	/* OBS owns the widget handed to obs_frontend_add_dock_by_id: removing the dock destroys it. */
	obs_frontend_remove_dock(kDockId);
	g_dock = nullptr;
}

#else

extern "C" void clip_director_midi_dock_load(void) {}

extern "C" void clip_director_midi_dock_unload(void) {}

#endif
