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
#include <QMainWindow>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "obs/cd-api.h"

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
		auto *root = new QVBoxLayout(this);
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
