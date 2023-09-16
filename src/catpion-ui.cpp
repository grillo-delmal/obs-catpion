#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>

#include <util/util.hpp>

#include <QMainWindow>
#include <QObject>
#include <QTimer>
#include <QFileDialog>

#include "catpion-ui.hpp"
#include "model.h"

#define QT_UTF8(str) QString::fromUtf8(str, -1)

CatpionUI *cui;

CatpionUI::CatpionUI(QWidget *parent)
	: QDialog(parent),
	  ui(new Ui_Catpion)
{
	ui->setupUi(this);
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	QObject::connect(ui->modelLoad, &QPushButton::clicked, this,
			 &CatpionUI::modelLoadButton);
	QObject::connect(ui->modelUnload, &QPushButton::clicked, this,
			 &CatpionUI::modelUnloadButton);
	QObject::connect(ui->buttonBox->button(QDialogButtonBox::Close),
			 &QPushButton::clicked, this, &CatpionUI::hide);
}

void CatpionUI::closeEvent(QCloseEvent *event)
{
}

static bool push_model_reload(void *data, obs_source_t *source)
{
	const char *name = obs_source_get_id(source);
	if(strcmp(name, "catpion_audio_input") == 0) {
		obs_source_update(source, NULL);
	}
	return true;
}

void CatpionUI::modelLoadButton()
{
    //blog(LOG_INFO, "modelLoadButton");
	
	QString extensions = QStringLiteral("*.april");;
	QString filter;

	if (!extensions.isEmpty()) {
		filter += obs_module_text("FileFilter.ModuleFile");
		filter += QStringLiteral(" (");
		filter += extensions;
		filter += QStringLiteral(")");
	}

	static std::string lastBrowsedDir;

	QString file =
		QFileDialog::getOpenFileName(
			this, 
			QT_UTF8(obs_module_text("AddScripts")),
			QT_UTF8(lastBrowsedDir.c_str()), 
			filter);
	if (file.isEmpty())
		return;

	lastBrowsedDir =
		QFileInfo(file).absolutePath().toUtf8().constData();

	QByteArray pathBytes = file.toUtf8();
	const char *path = pathBytes.constData();

	ModelNew(path);
	if(this->cur_model == ModelCurID()) {
	    blog(LOG_ERROR, "Fail loading model: %s", path);
		return;
	}
	if(ModelGet(this->cur_model) != NULL) ModelRelease(this->cur_model);
	this->cur_model = ModelCurID();
	ModelTake(this->cur_model);

	AprilASRModel model = ModelGet(this->cur_model);

	ui->modelName->setText(aam_get_name(model));
	ui->modelDesc->setText(aam_get_description(model));
	ui->modelLang->setText(aam_get_language(model));
	ui->modelRate->setText(QString("%1").arg(aam_get_sample_rate(model)));
	
	obs_enum_sources(push_model_reload, NULL);	
}

void CatpionUI::modelUnloadButton()
{
    //blog(LOG_INFO, "modelUnloadButton");
	if(ModelGet(this->cur_model) != NULL) ModelRelease(this->cur_model);
	ModelDelete();
	this->cur_model = ModelCurID();

	ui->modelName->setText("...");
	ui->modelDesc->setText("...");
	ui->modelLang->setText("...");
	ui->modelRate->setText("...");

	obs_enum_sources(push_model_reload, NULL);	
}

void CatpionUI::showHideDialog()
{
	if (!isVisible()) {
		setVisible(true);
		QTimer::singleShot(250, this, &CatpionUI::show);
	} else {
		setVisible(false);
		QTimer::singleShot(250, this, &CatpionUI::hide);
	}
}

extern "C" void InitCatpionUI()
{
    QAction *action = (QAction *)obs_frontend_add_tools_menu_qaction(
		obs_module_text("Catpion"));

	obs_frontend_push_ui_translation(obs_module_get_string);

	QMainWindow *window = (QMainWindow *)obs_frontend_get_main_window();
	cui = new CatpionUI(window);
	auto cb = []() {
		cui->showHideDialog();
	};
	obs_frontend_pop_ui_translation();

	action->connect(action, &QAction::triggered, cb);
}
