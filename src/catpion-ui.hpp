#include <QDialog>
#include <obs-module.h>
#include <util/platform.h>
#include <obs.hpp>
#include <memory>
#include "ui_catpion.h"

class CatpionUI : public QDialog {
	Q_OBJECT

public:
	std::unique_ptr<Ui_Catpion> ui;
    CatpionUI(QWidget *parent);

	void closeEvent(QCloseEvent *event) override;
	void modelLoad();
	void modelUnload();

public slots:
    void modelLoadButton();
    void modelUnloadButton();
	void showHideDialog();
private:
	size_t cur_model;
};
