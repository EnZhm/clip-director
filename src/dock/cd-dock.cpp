/*
Clip Director dock (P7): what is playing, what is next, the beat, and the controls a VJ needs
mid-set (Skip, Hold, Ban, polarity), plus the folder list (on/off, section tag, weight) and the
event log. Reads the source through cdapi (thread-safe); every edit is saved in the source's
settings.
*/
#include "cd-dock.h"

#if defined(ENABLE_FRONTEND_API) && defined(ENABLE_QT)

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "obs/cd-api.h"
#include "scene_pool.h"

namespace {

const char *kSectionNames[] = {"GROOVE", "BREAKDOWN", "DROP"};
const char *kSectionColours[] = {"#2d6a4f", "#1d4e89", "#9d0208"};
const char *kTagNames[] = {"Any", "Groove", "Breakdown", "Drop"};

QString leaf(const std::string &path)
{
	QString p = QString::fromStdString(path);
	while (p.endsWith('/') || p.endsWith('\\'))
		p.chop(1);
	const int cut = std::max(p.lastIndexOf('/'), p.lastIndexOf('\\'));
	return cut >= 0 ? p.mid(cut + 1) : p;
}

class ClipDirectorDock : public QWidget {
public:
	ClipDirectorDock() : QWidget(nullptr)
	{
		setObjectName("ClipDirectorDock");
		auto *tabs = new QTabWidget(this);
		auto *page = new QWidget();
		auto *root = new QVBoxLayout(page);
		root->setContentsMargins(6, 6, 6, 6);
		root->setSpacing(6);

		/* source picker + clock */
		auto *top = new QHBoxLayout();
		sources_ = new QComboBox();
		sources_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		sources_->setMinimumContentsLength(12);
		top->addWidget(sources_, 1);
		clock_ = new QLabel("-");
		top->addWidget(clock_);
		root->addLayout(top);

		/* section chip + beat lights */
		auto *beat = new QHBoxLayout();
		section_ = new QLabel("GROOVE");
		section_->setAlignment(Qt::AlignCenter);
		section_->setMinimumWidth(96);
		beat->addWidget(section_);
		for (auto *&l : lights_) {
			l = new QLabel();
			l->setFixedSize(18, 18);
			beat->addWidget(l);
		}
		beat->addStretch(1);
		state_ = new QLabel();
		beat->addWidget(state_);
		root->addLayout(beat);

		/* now playing */
		auto *now = new QFrame();
		now->setFrameShape(QFrame::StyledPanel);
		auto *ng = new QGridLayout(now);
		ng->setContentsMargins(6, 6, 6, 6);
		live_thumb_ = new QLabel();
		live_thumb_->setFixedSize(160, 90);
		live_thumb_->setStyleSheet("background:#000; color:#666;");
		ng->addWidget(live_thumb_, 0, 0, 3, 1);
		live_name_ = new QLabel("-");
		live_name_->setStyleSheet("font-weight:bold;");
		live_name_->setWordWrap(true);
		ng->addWidget(live_name_, 0, 1, 1, 2);
		live_detail_ = new QLabel();
		live_detail_->setWordWrap(true);
		ng->addWidget(live_detail_, 1, 1, 1, 2);
		polarity_ = new QComboBox();
		polarity_->addItems({"Invert: auto", "Invert: always", "Invert: never"});
		polarity_->setToolTip("Polarity for the clip on screen (saved per clip)");
		ng->addWidget(polarity_, 2, 1);
		ban_ = new QPushButton("Ban clip");
		ban_->setToolTip("Never play this clip again (saved), and skip it on the next beat");
		ng->addWidget(ban_, 2, 2);
		ng->setColumnStretch(1, 1);
		root->addWidget(now);

		/* next + controls */
		auto *nx = new QHBoxLayout();
		next_thumb_ = new QLabel();
		next_thumb_->setFixedSize(96, 54);
		next_thumb_->setStyleSheet("background:#000; color:#666;");
		nx->addWidget(next_thumb_);
		next_name_ = new QLabel("Next: -");
		next_name_->setWordWrap(true);
		nx->addWidget(next_name_, 1);
		auto *btns = new QVBoxLayout();
		skip_ = new QPushButton("Skip");
		skip_->setToolTip("Cut to the next clip on the next beat");
		hold_ = new QPushButton("Hold");
		hold_->setCheckable(true);
		hold_->setToolTip("Stay on this clip: no cuts, no stutter, until released");
		btns->addWidget(skip_);
		btns->addWidget(hold_);
		nx->addLayout(btns);
		root->addLayout(nx);

		/* folders + log */
		auto *split = new QSplitter(Qt::Vertical);
		folders_ = new QTableWidget(0, 5);
		folders_->setHorizontalHeaderLabels({"On", "Folder", "Clips", "Section", "Weight"});
		folders_->verticalHeader()->hide();
		folders_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
		for (int col : {0, 2, 3, 4})
			folders_->horizontalHeader()->setSectionResizeMode(col, QHeaderView::ResizeToContents);
		folders_->setSelectionMode(QAbstractItemView::NoSelection);
		split->addWidget(folders_);
		log_ = new QPlainTextEdit();
		log_->setReadOnly(true);
		log_->setMaximumBlockCount(400);
		log_->setLineWrapMode(QPlainTextEdit::NoWrap);
		log_->setStyleSheet("font-family:Consolas,monospace; font-size:11px;");
		split->addWidget(log_);
		folders_->setMinimumHeight(110);
		split->setStretchFactor(0, 1);
		split->setStretchFactor(1, 1);
		root->addWidget(split, 1);

		footer_ = new QLabel();
		footer_->setStyleSheet("color:#999;");
		root->addWidget(footer_);

		tabs->addTab(page, "Clips");
		tabs->addTab(build_syn_page(), "Synesthesia");
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 0, 0, 0);
		outer->addWidget(tabs);

		connect(sources_, &QComboBox::currentIndexChanged, this, [this](int) { select_current(); });
		connect(skip_, &QPushButton::clicked, this,
			[this] { with_source([](obs_source_t *s) { cdapi::skip(s); }); });
		connect(hold_, &QPushButton::toggled, this,
			[this](bool on) { with_source([on](obs_source_t *s) { cdapi::set_hold(s, on); }); });
		connect(ban_, &QPushButton::clicked, this,
			[this] { with_source([](obs_source_t *s) { cdapi::ban_live(s); }); });
		connect(polarity_, &QComboBox::activated, this, [this](int idx) {
			const std::string path = live_path_;
			with_source([&](obs_source_t *s) { cdapi::set_polarity(s, path, idx); });
		});

		auto *fast = new QTimer(this);
		fast->setInterval(50);
		connect(fast, &QTimer::timeout, this, [this] { refresh(); });
		fast->start();
		auto *slow = new QTimer(this);
		slow->setInterval(2000);
		connect(slow, &QTimer::timeout, this, [this] { list_sources(); });
		slow->start();
		list_sources();
	}

	~ClipDirectorDock() override { obs_weak_source_release(weak_); }

	void list_sources()
	{
		std::vector<std::string> names;
		obs_enum_sources(
			[](void *p, obs_source_t *s) {
				if (cdapi::is_clip_director(s))
					static_cast<std::vector<std::string> *>(p)->push_back(obs_source_get_name(s));
				return true;
			},
			&names);
		QStringList now;
		for (const auto &n : names)
			now << QString::fromStdString(n);
		QStringList had;
		for (int i = 0; i < sources_->count(); ++i)
			had << sources_->itemText(i);
		if (now == had)
			return;
		const QString keep = sources_->currentText();
		sources_->blockSignals(true);
		sources_->clear();
		sources_->addItems(now);
		const int at = now.indexOf(keep);
		sources_->setCurrentIndex(at >= 0 ? at : (now.isEmpty() ? -1 : 0));
		sources_->blockSignals(false);
		select_current();
	}

	void select_current()
	{
		obs_weak_source_release(weak_);
		weak_ = nullptr;
		folder_paths_.clear();
		log_seq_ = 0;
		log_->clear();
		const QString name = sources_->currentText();
		if (name.isEmpty())
			return;
		obs_source_t *s = obs_get_source_by_name(name.toUtf8().constData());
		if (s) {
			weak_ = obs_source_get_weak_source(s);
			obs_source_release(s);
		}
	}

	template<typename F> void with_source(F &&fn)
	{
		obs_source_t *s = weak_ ? obs_weak_source_get_source(weak_) : nullptr;
		if (!s)
			return;
		fn(s);
		obs_source_release(s);
	}

	void set_thumb(QLabel *l, const std::string &path, std::string &shown)
	{
		if (path == shown)
			return;
		shown = path;
		if (path.empty()) {
			l->clear();
			l->setText("no preview");
			l->setAlignment(Qt::AlignCenter);
			return;
		}
		auto it = thumbs_.find(path);
		if (it == thumbs_.end()) {
			QPixmap pm(QString::fromStdString(path));
			if (thumbs_.size() > 300)
				thumbs_.clear();
			it = thumbs_.emplace(path, pm).first;
		}
		l->setPixmap(it->second.scaled(l->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
	}

	void refresh()
	{
		cdapi::Status st;
		bool ok = false;
		with_source([&](obs_source_t *s) { ok = cdapi::status(s, st); });
		setEnabled(ok || sources_->count() > 0);
		if (!ok) {
			clock_->setText(sources_->count() ? "-" : "Add a Clip Director source");
			return;
		}

		/* clock + beat */
		clock_->setText(QString("%1 %2 BPM").arg(st.clock_pd ? "PD" : "free").arg(st.bpm, 0, 'f', 1));
		clock_->setToolTip(st.clock_pd ? "Following Palette Director's clock"
					       : "Palette Director not found: free-running at the fallback BPM");
		const int sec = std::clamp(st.section, 0, 2);
		section_->setText(kSectionNames[sec]);
		section_->setStyleSheet(QString("background:%1; color:white; font-weight:bold; padding:2px 6px; "
						"border-radius:3px;")
						.arg(kSectionColours[sec]));
		const double b = std::max(0.0, st.beats);
		const int in_bar = (int)std::floor(b) % 4;
		const double frac = b - std::floor(b);
		for (int i = 0; i < 4; ++i) {
			/* the current beat of the bar stays lit; it flashes bright on the beat itself */
			const char *col = "#343a40";
			if (i == in_bar)
				col = frac < 0.25 ? (i == 0 ? "#ffd166" : "#f8f9fa") : (i == 0 ? "#9c7a2b" : "#868e96");
			lights_[i]->setStyleSheet(QString("border-radius:9px; background:%1;").arg(col));
		}
		QString stt;
		if (st.hold)
			stt = "HOLD";
		else if (st.stutter)
			stt = "STUTTER";
		else if (st.next_cut_beat > b)
			stt = QString("cut in %1 beats").arg((int)std::ceil(st.next_cut_beat - b - 1e-6));
		state_->setText(stt);
		if (hold_->isChecked() != st.hold) {
			hold_->blockSignals(true);
			hold_->setChecked(st.hold);
			hold_->blockSignals(false);
		}

		/* now playing */
		if (st.live.valid) {
			live_name_->setText(QString::fromStdString(st.live.name));
			live_detail_->setText(QString("%1 · from %2s · x%3%4%5")
						      .arg(leaf(st.live.folder))
						      .arg(st.live.inpoint, 0, 'f', 1)
						      .arg(st.live.speed, 0, 'f', 2)
						      .arg(st.live.invert ? " · inverted" : "")
						      .arg(st.live.loops ? " · loops" : ""));
			set_thumb(live_thumb_, st.live.thumb, live_thumb_path_);
			live_path_ = st.live.path;
			if (polarity_->currentIndex() != st.live.polarity)
				polarity_->setCurrentIndex(st.live.polarity);
		} else {
			live_name_->setText("-");
			live_detail_->clear();
			set_thumb(live_thumb_, "", live_thumb_path_);
			live_path_.clear();
		}
		polarity_->setEnabled(st.live.valid);
		ban_->setEnabled(st.live.valid);

		const cdapi::ClipInfo &nx = st.next.valid ? st.next : st.drop;
		if (nx.valid) {
			next_name_->setText(QString("%1%2\n%3%4")
						    .arg(&nx == &st.drop ? "Drop clip: " : "Next: ")
						    .arg(QString::fromStdString(nx.name))
						    .arg(leaf(nx.folder))
						    .arg(nx.ready ? "" : " · loading"));
			set_thumb(next_thumb_, nx.thumb, next_thumb_path_);
		} else {
			next_name_->setText("Next: -");
			set_thumb(next_thumb_, "", next_thumb_path_);
		}

		/* folders (rebuilt only when the list changes; editors left alone while in use) */
		std::vector<std::string> paths;
		for (const auto &f : st.folders)
			paths.push_back(f.path);
		if (paths != folder_paths_)
			build_folders(st);
		for (int r = 0; r < (int)st.folders.size() && r < folders_->rowCount(); ++r) {
			if (auto *it = folders_->item(r, 2))
				it->setText(QString::number(st.folders[r].clips) +
					    (st.folders[r].has_index ? "" : " raw"));
		}

		/* log */
		if (st.log_seq != log_seq_) {
			const uint64_t fresh = st.log_seq - log_seq_;
			const size_t from = fresh >= st.log.size() ? 0 : st.log.size() - (size_t)fresh;
			auto *sb = log_->verticalScrollBar();
			const bool at_end = sb->value() >= sb->maximum() - 4;
			for (size_t i = from; i < st.log.size(); ++i)
				log_->appendPlainText(QString::fromStdString(st.log[i]));
			if (at_end)
				sb->setValue(sb->maximum());
			log_seq_ = st.log_seq;
		}

		refresh_syn(st.syn);

		footer_->setText(QString("%1 clips (%2 with stats, %3 banned) · %4 cuts · timing %5 ms")
					 .arg(st.clips)
					 .arg(st.with_stats)
					 .arg(st.banned)
					 .arg(st.cuts)
					 .arg(st.cut_err_mean_ms, 0, 'f', 1));
	}

	void build_folders(const cdapi::Status &st)
	{
		folder_paths_.clear();
		for (const auto &f : st.folders)
			folder_paths_.push_back(f.path);
		folders_->setRowCount((int)st.folders.size());
		for (int r = 0; r < (int)st.folders.size(); ++r) {
			const auto &f = st.folders[r];
			auto *on = new QCheckBox();
			on->setChecked(f.enabled);
			auto *name = new QTableWidgetItem(leaf(f.path));
			name->setToolTip(QString::fromStdString(f.path));
			name->setFlags(Qt::ItemIsEnabled);
			auto *count = new QTableWidgetItem();
			count->setFlags(Qt::ItemIsEnabled);
			auto *tag = new QComboBox();
			for (const char *t : kTagNames)
				tag->addItem(t);
			tag->setCurrentIndex(std::clamp(f.tag, 0, 3));
			tag->setToolTip("Which sections play this folder (Any = groove, and the fallback)");
			auto *w = new QDoubleSpinBox();
			w->setRange(0.0, 10.0);
			w->setSingleStep(0.5);
			w->setDecimals(1);
			w->setValue(f.weight);
			w->setToolTip("How often this folder is picked relative to the others (0 = never)");
			folders_->setCellWidget(r, 0, on);
			folders_->setItem(r, 1, name);
			folders_->setItem(r, 2, count);
			folders_->setCellWidget(r, 3, tag);
			folders_->setCellWidget(r, 4, w);
			auto push = [this, r, on, tag, w] {
				with_source([&](obs_source_t *s) {
					cdapi::set_folder(s, r, tag->currentIndex(), w->value(), on->isChecked());
				});
			};
			connect(on, &QCheckBox::toggled, this, push);
			connect(tag, &QComboBox::activated, this, push);
			connect(w, &QDoubleSpinBox::editingFinished, this, push);
		}
	}

	/* ------------------------------------------------------------------------- */
	/* Synesthesia tab (W2/W3)                                                   */

	QWidget *build_syn_page()
	{
		auto *w = new QWidget();
		auto *v = new QVBoxLayout(w);
		v->setContentsMargins(6, 6, 6, 6);
		v->setSpacing(6);

		/* link: Auto, the link chip, host and ports */
		auto *link = new QHBoxLayout();
		syn_auto_ = new QPushButton("Auto OFF");
		syn_auto_->setCheckable(true);
		syn_auto_->setToolTip("Plan scene launches with the cuts and send them over OSC (needs "
				      "Synesthesia Pro with OSC input on)");
		link->addWidget(syn_auto_);
		syn_chip_ = new QLabel("OSC off");
		syn_chip_->setAlignment(Qt::AlignCenter);
		syn_chip_->setMinimumWidth(104);
		link->addWidget(syn_chip_);
		syn_host_ = new QLineEdit();
		syn_host_->setMaximumWidth(110);
		syn_host_->setToolTip("Where Synesthesia listens (Settings > OSC > Input port)");
		link->addWidget(syn_host_);
		syn_send_ = new QSpinBox();
		syn_send_->setRange(1, 65535);
		syn_send_->setToolTip("Synesthesia's OSC input port");
		link->addWidget(syn_send_);
		syn_fb_ = new QSpinBox();
		syn_fb_->setRange(1, 65535);
		syn_fb_->setToolTip("Synesthesia's OSC output port (Settings > OSC > Output): its echoes "
				    "come back here and light the link chip");
		link->addWidget(syn_fb_);
		link->addStretch(1);
		v->addLayout(link);

		/* now / next scene */
		auto *now = new QFrame();
		now->setFrameShape(QFrame::StyledPanel);
		auto *ng = new QGridLayout(now);
		ng->setContentsMargins(6, 6, 6, 6);
		auto *nl = new QLabel("Synesthesia now");
		nl->setStyleSheet("color:#888;");
		ng->addWidget(nl, 0, 0, 1, 2);
		syn_now_ = new QLabel("-");
		syn_now_->setStyleSheet("font-weight:bold;");
		syn_now_->setWordWrap(true);
		ng->addWidget(syn_now_, 1, 0, 1, 2);
		syn_next_ = new QLabel("-");
		syn_next_->setWordWrap(true);
		ng->addWidget(syn_next_, 2, 0, 1, 2);
		ng->setColumnStretch(0, 1);
		v->addWidget(now);

		/* manual controls */
		auto *man = new QHBoxLayout();
		syn_next_btn_ = new QPushButton("Next scene");
		syn_next_btn_->setToolTip("Launch the next scene of the pool for the current section");
		syn_hold_btn_ = new QPushButton("Hold scene");
		syn_hold_btn_->setCheckable(true);
		syn_hold_btn_->setToolTip("Suspend the automatic launches (clip cuts keep running)");
		syn_bang_btn_ = new QPushButton("Fire bang 1");
		syn_bang_btn_->setToolTip("Send /controls/global/bang/1 1.0");
		man->addWidget(syn_next_btn_);
		man->addWidget(syn_hold_btn_);
		man->addWidget(syn_bang_btn_);
		v->addLayout(man);

		/* when to switch */
		auto *t = new QLabel("When to switch");
		t->setStyleSheet("color:#888;");
		v->addWidget(t);
		auto *grid = new QGridLayout();
		syn_on_bd_ = new QComboBox();
		syn_on_bd_->addItems({"Launch from Breakdown pool", "Do nothing"});
		syn_on_drop_ = new QComboBox();
		syn_on_drop_->addItems({"Launch from Drop pool", "Do nothing"});
		syn_groove_ = new QComboBox();
		syn_groove_->addItems({"Never", "Every 2 phrases", "Every 4 phrases"});
		syn_bang_drop_ = new QComboBox();
		syn_bang_drop_->addItems({"Nothing", "Scene bang 1"});
		grid->addWidget(new QLabel("Breakdown starts"), 0, 0);
		grid->addWidget(syn_on_bd_, 0, 1);
		grid->addWidget(new QLabel("Drop downbeat"), 1, 0);
		grid->addWidget(syn_on_drop_, 1, 1);
		grid->addWidget(new QLabel("During groove"), 2, 0);
		grid->addWidget(syn_groove_, 2, 1);
		grid->addWidget(new QLabel("Drop: also fire"), 3, 0);
		grid->addWidget(syn_bang_drop_, 3, 1);
		grid->addWidget(new QLabel("Scene lead"), 4, 0);
		auto *leadrow = new QHBoxLayout();
		syn_lead_ = new QSlider(Qt::Horizontal);
		syn_lead_->setRange(0, 500);
		syn_lead_->setToolTip("Send phrase-aligned groove launches this much early, to hide "
				      "Synesthesia's scene-load time (breakdown and drop are detected, "
				      "never predicted)");
		syn_lead_lbl_ = new QLabel("0 ms");
		syn_lead_lbl_->setMinimumWidth(52);
		leadrow->addWidget(syn_lead_, 1);
		leadrow->addWidget(syn_lead_lbl_);
		grid->addLayout(leadrow, 4, 1);
		grid->setColumnStretch(1, 1);
		v->addLayout(grid);

		/* scene pools */
		auto *ph = new QHBoxLayout();
		auto *pl = new QLabel("Scene pools");
		pl->setStyleSheet("color:#888;");
		ph->addWidget(pl);
		syn_pool_ = new QComboBox();
		syn_pool_->addItems({"Groove", "Breakdown", "Drop"});
		ph->addWidget(syn_pool_);
		auto *add = new QPushButton("Add");
		auto *rm = new QPushButton("Remove");
		auto *up = new QPushButton("Up");
		auto *down = new QPushButton("Down");
		syn_import_ = new QPushButton("Import from Synesthesia");
		syn_import_->setToolTip("Add every scene name Synesthesia has echoed (needs OSC output on)");
		ph->addWidget(add);
		ph->addWidget(rm);
		ph->addWidget(up);
		ph->addWidget(down);
		ph->addWidget(syn_import_);
		ph->addStretch(1);
		v->addLayout(ph);

		syn_pools_table_ = new QTableWidget(0, 2);
		syn_pools_table_->setHorizontalHeaderLabels({"Scene", "Preset"});
		syn_pools_table_->verticalHeader()->hide();
		syn_pools_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
		syn_pools_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
		syn_pools_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
		syn_pools_table_->setMinimumHeight(96);
		syn_pools_table_->setToolTip("Double-click a row to launch that scene now");
		v->addWidget(syn_pools_table_, 1);

		syn_log_ = new QPlainTextEdit();
		syn_log_->setReadOnly(true);
		syn_log_->setMaximumBlockCount(400);
		syn_log_->setLineWrapMode(QPlainTextEdit::NoWrap);
		syn_log_->setStyleSheet("font-family:Consolas,monospace; font-size:11px;");
		syn_log_->setMinimumHeight(90);
		v->addWidget(syn_log_, 1);

		/* wiring */
		connect(syn_auto_, &QPushButton::toggled, this, [this](bool on) {
			syn_auto_->setText(on ? "Auto ON" : "Auto OFF");
			with_source([on](obs_source_t *s) { cdapi::syn_set_enabled(s, on); });
		});
		connect(syn_host_, &QLineEdit::editingFinished, this, [this] { push_link(); });
		connect(syn_send_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { push_link(); });
		connect(syn_fb_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { push_link(); });
		connect(syn_next_btn_, &QPushButton::clicked, this,
			[this] { with_source([](obs_source_t *s) { cdapi::syn_next(s); }); });
		connect(syn_hold_btn_, &QPushButton::toggled, this,
			[this](bool on) { with_source([on](obs_source_t *s) { cdapi::syn_hold(s, on); }); });
		connect(syn_bang_btn_, &QPushButton::clicked, this,
			[this] { with_source([](obs_source_t *s) { cdapi::syn_bang(s); }); });
		for (QComboBox *c : {syn_on_bd_, syn_on_drop_, syn_groove_, syn_bang_drop_})
			connect(c, &QComboBox::activated, this, [this](int) { push_trigger(); });
		connect(syn_lead_, &QSlider::valueChanged, this,
			[this](int v) { syn_lead_lbl_->setText(QString("%1 ms").arg(v)); });
		connect(syn_lead_, &QSlider::sliderReleased, this, [this] { push_trigger(); });
		connect(syn_pool_, &QComboBox::currentIndexChanged, this, [this](int) { rebuild_pools(); });
		connect(add, &QPushButton::clicked, this, [this] {
			pools_.of(pool_at(syn_pool_->currentIndex())).push_back(cd::SceneEntry());
			push_pools();
			rebuild_pools();
			const int r = syn_pools_table_->rowCount() - 1;
			if (r >= 0 && syn_pools_table_->item(r, 0))
				syn_pools_table_->editItem(syn_pools_table_->item(r, 0));
		});
		connect(rm, &QPushButton::clicked, this, [this] {
			const int r = syn_pools_table_->currentRow();
			auto &pool = pools_.of(pool_at(syn_pool_->currentIndex()));
			if (r < 0 || r >= (int)pool.size())
				return;
			pool.erase(pool.begin() + r);
			push_pools();
			rebuild_pools();
		});
		connect(up, &QPushButton::clicked, this, [this] { move_pool_row(-1); });
		connect(down, &QPushButton::clicked, this, [this] { move_pool_row(1); });
		connect(syn_pools_table_, &QTableWidget::cellChanged, this, [this](int r, int col) {
			auto &pool = pools_.of(pool_at(syn_pool_->currentIndex()));
			if (rebuilding_ || r < 0 || r >= (int)pool.size())
				return;
			auto *it = syn_pools_table_->item(r, col);
			if (!it)
				return;
			(col == 0 ? pool[r].scene : pool[r].preset) = it->text().toStdString();
			push_pools();
		});
		connect(syn_pools_table_, &QTableWidget::cellDoubleClicked, this, [this](int r, int) {
			push_pools();
			const int pool = syn_pool_->currentIndex();
			with_source([pool, r](obs_source_t *s) { cdapi::syn_launch(s, pool, r); });
		});
		connect(syn_import_, &QPushButton::clicked, this, [this] {
			cdapi::SynInfo syn;
			with_source([&](obs_source_t *s) {
				cdapi::Status st;
				if (cdapi::status(s, st))
					syn = st.syn;
			});
			if (syn.heard.empty()) {
				syn_log_->appendPlainText("import: nothing heard from Synesthesia yet "
							  "(turn its OSC output on)");
				return;
			}
			auto &pool = pools_.of(pool_at(syn_pool_->currentIndex()));
			int added = 0;
			for (const std::string &name : syn.heard) {
				bool have = false;
				for (const auto &e : pool)
					have |= e.scene == name;
				if (!have) {
					cd::SceneEntry e;
					e.scene = name;
					pool.push_back(e);
					added++;
				}
			}
			push_pools();
			rebuild_pools();
			syn_log_->appendPlainText(QString("import: %1 scene(s) added to the %2 pool")
							  .arg(added)
							  .arg(syn_pool_->currentText()));
		});
		rebuild_pools();
		return w;
	}

	void push_link()
	{
		const std::string host = syn_host_->text().toStdString();
		const int send = syn_send_->value(), fb = syn_fb_->value();
		with_source([&](obs_source_t *s) { cdapi::syn_set_link(s, host, send, fb); });
	}

	void push_trigger()
	{
		const bool bd = syn_on_bd_->currentIndex() == 0;
		const bool dr = syn_on_drop_->currentIndex() == 0;
		const int phrases = syn_groove_->currentIndex() == 1 ? 2 : (syn_groove_->currentIndex() == 2 ? 4 : 0);
		const bool bang = syn_bang_drop_->currentIndex() == 1;
		const double lead = syn_lead_->value();
		with_source([&](obs_source_t *s) { cdapi::syn_set_trigger(s, bd, dr, phrases, lead, bang); });
	}

	void push_pools()
	{
		const std::string json = cd::scene_pools_to_json(pools_);
		if (json == pools_json_)
			return;
		pools_json_ = json;
		with_source([&](obs_source_t *s) { cdapi::syn_set_pools(s, json); });
	}

	void move_pool_row(int delta)
	{
		const int r = syn_pools_table_->currentRow();
		auto &pool = pools_.of(pool_at(syn_pool_->currentIndex()));
		const int to = r + delta;
		if (r < 0 || r >= (int)pool.size() || to < 0 || to >= (int)pool.size())
			return;
		std::swap(pool[(size_t)r], pool[(size_t)to]);
		push_pools();
		rebuild_pools();
		syn_pools_table_->setCurrentCell(to, 0);
	}

	/* the table shows the selected pool's entries from the dock's own copy */
	void rebuild_pools()
	{
		const std::vector<cd::SceneEntry> &pool = pools_.of(pool_at(syn_pool_->currentIndex()));
		rebuilding_ = true;
		syn_pools_table_->setRowCount((int)pool.size());
		for (int r = 0; r < (int)pool.size(); ++r) {
			syn_pools_table_->setItem(r, 0, new QTableWidgetItem(QString::fromStdString(pool[r].scene)));
			syn_pools_table_->setItem(r, 1, new QTableWidgetItem(QString::fromStdString(pool[r].preset)));
		}
		rebuilding_ = false;
	}

	void refresh_syn(const cdapi::SynInfo &sy)
	{
		/* Auto / link chip */
		if (syn_auto_->isChecked() != sy.auto_on) {
			syn_auto_->blockSignals(true);
			syn_auto_->setChecked(sy.auto_on);
			syn_auto_->setText(sy.auto_on ? "Auto ON" : "Auto OFF");
			syn_auto_->blockSignals(false);
		}
		QString chip, col;
		if (!sy.enabled) {
			chip = "OSC off";
			col = "#495057";
		} else if (sy.linked) {
			chip = "OSC \u25cf linked";
			col = "#1f6a45";
		} else if (!sy.running) {
			chip = "OSC error";
			col = "#7f1d1d";
		} else {
			chip = "OSC \u25cf sending";
			col = "#9a6b00";
		}
		syn_chip_->setText(chip);
		syn_chip_->setStyleSheet(
			QString("background:%1; color:white; font-weight:bold; padding:2px 6px; border-radius:3px;")
				.arg(col));
		syn_chip_->setToolTip(
			QString("%1  sent %2  received %3%4")
				.arg(sy.error.empty() ? QString::fromStdString(sy.host)
						      : QString::fromStdString(sy.error))
				.arg(sy.sent)
				.arg(sy.received)
				.arg(sy.since_echo_ms >= 0
					     ? QString(", echo %1 s ago").arg(sy.since_echo_ms / 1000.0, 0, 'f', 1)
					     : QString(", no echo yet")));
		if (sy.since_echo_ms < 0 && sy.sent > 0)
			syn_chip_->setToolTip(syn_chip_->toolTip() + "\nNo echo: is Synesthesia's OSC output on, "
								     "with this feedback port?");
		if (!sy.host.empty() && syn_host_->text().toStdString() != sy.host)
			syn_host_->setText(QString::fromStdString(sy.host));
		if (syn_send_->value() != sy.send_port)
			syn_send_->setValue(sy.send_port);
		if (syn_fb_->value() != sy.feedback_port)
			syn_fb_->setValue(sy.feedback_port);

		/* now / next */
		const QString nowp =
			sy.now_preset.empty()
				? QString()
				: QString(" \u00b7 \u201c%1\u201d").arg(QString::fromStdString(sy.now_preset));
		syn_now_->setText(sy.now_scene.empty() ? QString("-") : QString::fromStdString(sy.now_scene) + nowp);
		QString nxt = "Next: ";
		if (sy.next_scene.empty())
			nxt += "no scene in the pools";
		else {
			nxt += QString::fromStdString(sy.next_scene);
			if (!sy.next_preset.empty())
				nxt += QString(" \u00b7 \u201c%1\u201d").arg(QString::fromStdString(sy.next_preset));
		}
		if (sy.next_scene_beat > 0.0)
			nxt += QString("  (groove plan at beat %1)").arg(sy.next_scene_beat, 0, 'f', 1);
		syn_next_->setText(nxt);
		if (sy.hold != syn_hold_btn_->isChecked()) {
			syn_hold_btn_->blockSignals(true);
			syn_hold_btn_->setChecked(sy.hold);
			syn_hold_btn_->blockSignals(false);
		}

		/* the trigger options (the source is the authority; the dock just shows them) */
		set_combo(syn_on_bd_, sy.on_breakdown ? 0 : 1);
		set_combo(syn_on_drop_, sy.on_drop ? 0 : 1);
		set_combo(syn_groove_, sy.groove_phrases == 4 ? 2 : (sy.groove_phrases == 2 ? 1 : 0));
		set_combo(syn_bang_drop_, sy.bang_on_drop ? 1 : 0);
		if (syn_lead_->value() != (int)std::lround(sy.lead_ms)) {
			syn_lead_->blockSignals(true);
			syn_lead_->setValue((int)std::lround(sy.lead_ms));
			syn_lead_->blockSignals(false);
			syn_lead_lbl_->setText(QString("%1 ms").arg((int)std::lround(sy.lead_ms)));
		}

		/* the pools come back from the source settings; the dock only rebuilds on a change */
		if (sy.pools_json != pools_json_) {
			cd::ScenePools parsed;
			if (sy.pools_json.empty())
				parsed = cd::ScenePools();
			else if (!cd::scene_pools_from_json(sy.pools_json, parsed))
				parsed = cd::ScenePools();
			pools_ = parsed;
			pools_json_ = sy.pools_json;
			rebuild_pools();
		}

		/* the OSC log */
		if (sy.log_seq != syn_log_seq_) {
			const uint64_t fresh = sy.log_seq - syn_log_seq_;
			const size_t from = fresh >= sy.log.size() ? 0 : sy.log.size() - (size_t)fresh;
			auto *sb = syn_log_->verticalScrollBar();
			const bool at_end = sb->value() >= sb->maximum() - 4;
			for (size_t i = from; i < sy.log.size(); ++i)
				syn_log_->appendPlainText(QString::fromStdString(sy.log[i]));
			if (at_end)
				sb->setValue(sb->maximum());
			syn_log_seq_ = sy.log_seq;
		}
	}

	static void set_combo(QComboBox *c, int index)
	{
		if (!c || c->currentIndex() == index)
			return;
		c->blockSignals(true);
		c->setCurrentIndex(index);
		c->blockSignals(false);
	}

	/* the pool combo index maps straight onto cd::Section (0 groove, 1 breakdown, 2 drop) */
	static cd::Section pool_at(int index) { return static_cast<cd::Section>(std::clamp(index, 0, 2)); }

private:
	obs_weak_source_t *weak_ = nullptr;
	QComboBox *sources_, *polarity_;
	QLabel *clock_, *section_, *state_, *lights_[4];
	QLabel *live_thumb_, *live_name_, *live_detail_, *next_thumb_, *next_name_, *footer_;
	QPushButton *skip_, *hold_, *ban_;
	QTableWidget *folders_;
	QPlainTextEdit *log_;
	std::vector<std::string> folder_paths_;
	std::string live_path_, live_thumb_path_, next_thumb_path_;
	std::map<std::string, QPixmap> thumbs_;
	uint64_t log_seq_ = 0;

	/* Synesthesia tab */
	QPushButton *syn_auto_, *syn_next_btn_, *syn_hold_btn_, *syn_bang_btn_, *syn_import_;
	QLabel *syn_chip_, *syn_now_, *syn_next_, *syn_lead_lbl_;
	QLineEdit *syn_host_;
	QSpinBox *syn_send_, *syn_fb_;
	QComboBox *syn_on_bd_, *syn_on_drop_, *syn_groove_, *syn_bang_drop_, *syn_pool_;
	QSlider *syn_lead_;
	QTableWidget *syn_pools_table_;
	QPlainTextEdit *syn_log_;
	cd::ScenePools pools_;   // the dock's copy, pushed into syn_pools as a JSON string
	std::string pools_json_; // what the source settings hold (as last seen)
	bool rebuilding_ = false;
	uint64_t syn_log_seq_ = 0;
};

ClipDirectorDock *g_dock = nullptr;

} // namespace

extern "C" void clip_director_dock_load(void)
{
	auto *dock = new ClipDirectorDock();
	if (!obs_frontend_add_dock_by_id("clip-director-dock", obs_module_text("ClipDirector"), dock)) {
		obs_log(LOG_ERROR, "dock id already in use; dock not added");
		delete dock;
		return;
	}
	g_dock = dock;
	obs_log(LOG_INFO, "dock registered");

#if defined(CD_TEST_HOOKS)
	/* sandbox only (-DCD_TEST_HOOKS=ON, never in releases): CD_DOCK_SNAPSHOT=<png path> opens the
	 * dock docked right and saves a grab of it every 2 s */
	const QString snap = qEnvironmentVariable("CD_DOCK_SNAPSHOT");
	if (!snap.isEmpty()) {
		obs_log(LOG_INFO, "test hooks: dock snapshots to %s", snap.toUtf8().constData());
		QTimer::singleShot(8000, dock, [dock] {
			auto *dw = qobject_cast<QDockWidget *>(dock->parentWidget());
			auto *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window());
			if (!dw || !mw)
				return;
			dw->setFloating(false);
			mw->addDockWidget(Qt::RightDockWidgetArea, dw);
			dw->show();
			dw->raise();
			mw->resizeDocks({dw}, {460}, Qt::Horizontal);
		});
		auto *t = new QTimer(dock);
		t->setInterval(2000);
		QObject::connect(t, &QTimer::timeout, dock, [dock, snap] { dock->grab().save(snap); });
		t->start();
	}
#endif
}

#else

extern "C" void clip_director_dock_load(void) {}

#endif
