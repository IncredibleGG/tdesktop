/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_fake_crash_settings.h"

#include "base/openssl_help.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "lang/lang_keys.h"
#include "lumina/lumina_fake_crash.h"
#include "lumina/lumina_locale.h"
#include "lumina/lumina_vault.h"
#include "main/main_domain.h"
#include "settings/settings_common.h"
#include "storage/storage_domain.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

namespace Lumina {
namespace {

// Long enough for a passphrase, short enough that the row label stays honest.
constexpr auto kCodeMaxLength = 64;

// Every label, toggle and paragraph on the block is recomputed from the store
// on any change to it, to the local passcode and to the disguise vault - the
// last two matter because some of the warnings below are about them, and both
// are edited elsewhere. LangChanges() is merged in because every text they
// produce is itself a translated string.
[[nodiscard]] rpl::producer<> Changes() {
	return rpl::merge(
		FakeCrashChanges(),
		Core::App().domain().local().localPasscodeChanged(),
		VaultChanges(),
		LangChanges());
}

[[nodiscard]] rpl::producer<QString> LabelValue(Fn<QString()> compute) {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		Changes()
	) | rpl::map([compute = std::move(compute)] {
		return compute();
	});
}

[[nodiscard]] bool HasLocalPasscode() {
	const auto &domain = Core::App().domain();
	return domain.started() && domain.local().hasLocalPasscode();
}

// The duress passcode and the vault's secret code are typed into the same
// gate; the vault's always wins there (lumina_vault_door.cpp), so equal codes
// would silently cost the duress passcode at the vault.
[[nodiscard]] bool SameAsVaultCode(const QString &code) {
	return !code.isEmpty() && VaultCodeIsSet() && (code == VaultCode());
}

// Answers done(true) when `code` would unlock the app, on the main thread and
// only while `guard` is alive.
//
// Since 7.3 the passcode check is a memory-hard derivation that upstream never
// runs on the main thread: Storage::Domain::checkPasscode() takes a prepared
// PasscodeDerivation, and the derivation itself runs on a worker through
// Storage::DeriveOnWorker() - the same way the lock screen and the passcode
// settings page check it now. So this answers later, not at once.
//
// checkPasscode() asserts on key material that only exists once the domain
// has started (Expects(_localKey) in storage/storage_domain.cpp), and there is
// nothing to collide with when no passcode lock is set at all - hence both
// guards, not just the obvious one, checked again when the verdict arrives.
// hasLocalPasscode() now means "the app lock is armed": a passcode kept only
// for the wallet never shows the lock screen, so it cannot collide with a
// duress code typed there either.
void CheckCodeIsLocalPasscode(
		const QString &code,
		not_null<QObject*> guard,
		Fn<void(bool)> done) {
	if (code.isEmpty() || !HasLocalPasscode()) {
		done(false);
		return;
	}
	// Same conversion the lock screen applies to what the user typed
	// (window/window_lock_widgets.cpp, TryPasscode), cleansed the same way.
	auto utf8 = code.toUtf8();
	const auto cleanse = gsl::finally([&] {
		if (!utf8.isEmpty()) {
			OPENSSL_cleanse(utf8.data(), utf8.size());
		}
	});
	const auto &local = Core::App().domain().local();
	Storage::DeriveOnWorker(
		local.prepareOpen(utf8),
		crl::guard(guard, [done](Storage::PasscodeDerivation &&derived) {
			done(HasLocalPasscode()
				&& Core::App().domain().local().checkPasscode(
					std::move(derived)));
		}));
}

[[nodiscard]] QString ModeName(DuressMode mode) {
	switch (mode) {
	case DuressMode::LogoutCurrent:
		return Tr(u"LuminaDuressModeLogoutCurrent"_q);
	case DuressMode::LogoutAll:
		return Tr(u"LuminaDuressModeLogoutAll"_q);
	case DuressMode::Exit:
		break;
	}
	return Tr(u"LuminaFakeCrashModeExit"_q);
}

// The owner's own warning for each logout mode: what triggering it costs, and
// what to check first so the account can be signed back into.
[[nodiscard]] QString LogoutWarning(DuressMode mode) {
	return Tr((mode == DuressMode::LogoutAll)
		? u"LuminaDuressLogoutAllWarning"_q
		: u"LuminaDuressLogoutCurrentWarning"_q);
}

// The explicit confirmation every logout mode goes through before it can be
// armed: picking it below is the only way to arm one. Switching the feature
// on starts at Exit, the fake-crash era's users start off, a missing or
// unknown mode reads as off, and a restored backup never carries a logout
// mode (FilterDuressBackupPayload()).
//
// `done(true)` once confirmed; `done(false)` however else the box goes away
// (Cancel, Escape, a click outside - Ui::ConfirmBox reports all of them as a
// cancel unless strictCancel is set, which it is not here).
void ConfirmLogoutMode(
		not_null<Window::SessionController*> controller,
		DuressMode mode,
		Fn<void(bool)> done) {
	const auto text = LogoutWarning(mode)
		+ u"\n\n"_q
		+ Tr(u"LuminaDuressLocalDataWarning"_q);
	controller->show(Ui::MakeConfirmBox({
		.text = text,
		.confirmed = [=](Fn<void()> close) {
			done(true);
			close();
		},
		.cancelled = [=](Fn<void()> close) {
			done(false);
			close();
		},
		.confirmText = Tr(u"LuminaDuressConfirmButton"_q),
		.confirmStyle = &st::attentionBoxButton,
		.title = ModeName(mode),
	}));
}

// The feature switch. On means Exit, the one mode that destroys nothing; off
// forgets the mode (the code is kept), so a logout mode is always chosen
// again, through its confirmation, after the feature was off - the same as on
// Android, where "off" is one more value of the same preference. The guard on
// toggledChanges() keeps every programmatic flip - a restored backup, a panic
// wipe - from being written straight back into the store.
void AddEnableRow(not_null<Ui::VerticalLayout*> container) {
	const auto button = container->add(object_ptr<Ui::SettingsButton>(
		container,
		TrValue(u"LuminaSecurityFakeCrashEnable"_q),
		st::settingsButtonNoIcon
	))->toggleOn(rpl::single(
		rpl::empty
	) | rpl::then(
		Changes()
	) | rpl::map([] {
		return FakeCrashEnabled();
	}));
	button->toggledChanges(
	) | rpl::filter([](bool value) {
		return (value != FakeCrashEnabled());
	}) | rpl::on_next([](bool value) {
		SetFakeCrashEnabled(value);
	}, button->lifetime());
}

// `Settings` names Lumina::Settings inside this namespace, so the settings
// section helpers have to be reached through the global namespace.
void AddValueRow(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> label,
		Fn<QString()> value,
		Fn<void()> activate) {
	::Settings::AddButtonWithLabel(
		container,
		std::move(label),
		LabelValue(std::move(value)),
		st::settingsButtonNoIcon
	)->setClickedCallback(std::move(activate));
}

void EditCodeBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(TrValue(u"LuminaSecurityFakeCrashCodeDialogTitle"_q));

	// Shown in the clear, deliberately, and unlike Android - which masks the
	// field. The row behind this box only says "Set", so without this there is
	// no way to find out what the code actually is, and a duress code the user
	// is no longer sure of is a duress code they will not use. It is the same
	// trade the translation page already makes for API keys: masked in the
	// row, legible in the editor. Anyone who can read this box is looking at
	// an unlocked LuminaGram anyway.
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::SingleLine,
		TrValue(u"LuminaSecurityFakeCrashCodeHint"_q),
		FakeCrashCode()));
	field->setMaxLength(kCodeMaxLength);
	box->setFocusCallback([=] {
		field->setFocusFast();
	});

	// The passcode comparison below derives on a worker (see
	// CheckCodeIsLocalPasscode), so a second Save while it runs is ignored
	// rather than starting a second derivation.
	const auto checking = box->lifetime().make_state<bool>(false);
	const auto submit = [=] {
		if (*checking) {
			return;
		}
		const auto entered = field->getLastText().trimmed();

		// Nor the vault's secret code: at the disguise gate the vault's own
		// code always wins, so the duress passcode would open the app it is
		// meant to close.
		if (SameAsVaultCode(entered)) {
			box->uiShow()->showBox(Ui::MakeInformBox(
				Tr(u"LuminaSecurityFakeCrashCodeSameAsVault"_q)));
			return;
		}

		// The duress code must not be the passcode. The lock screen would
		// still behave sanely if it were - it only consults the duress code
		// after the passcode has already been rejected, so a collision costs
		// the feature there, not the account - but the disguise vault cannot
		// check the passcode at all, so in a logout mode typing the passcode
		// there would log out. The passcode page refuses the same collision
		// from its side (settings_local_passcode.cpp).
		*checking = true;
		CheckCodeIsLocalPasscode(entered, box, [=](bool clash) {
			*checking = false;
			if (clash) {
				box->uiShow()->showBox(Ui::MakeInformBox(
					Tr(u"LuminaSecurityFakeCrashCodeSameAsPasscode"_q)));
				return;
			}
			SetFakeCrashCode(entered);
			box->closeBox();
		});
	};
	field->submits() | rpl::on_next(submit, field->lifetime());

	box->addButton(tr::lng_settings_save(), submit);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

// "When this code is entered": the three modes, as radio rows inside the
// block that only shows while the feature is on. Placed under the code row
// rather than above it, so the code row is not read as another choice; Exit
// is listed first because switching the feature on selects it, and it is the
// only one that destroys nothing.
void AddModeRows(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		TrValue(u"LuminaFakeCrashModeHeader"_q));
	Ui::AddSkip(container, st::settingsSendTypeSkip);

	const auto group = std::make_shared<Ui::RadioenumGroup<DuressMode>>(
		CurrentDuressMode());
	const auto add = [&](DuressMode mode) {
		const auto row = container->add(
			object_ptr<Ui::Radioenum<DuressMode>>(
				container,
				group,
				mode,
				ModeName(mode),
				st::settingsSendType),
			st::settingsSendTypePadding);
		LangChanges(
		) | rpl::on_next([=] {
			row->setText(ModeName(mode));
		}, row->lifetime());
	};
	add(DuressMode::Exit);
	add(DuressMode::LogoutCurrent);
	add(DuressMode::LogoutAll);

	// The preference can also move under the page - a restored backup writes
	// every key it carries - so the rows follow the preference rather than
	// only the clicks; `syncing` keeps that path, and a cancelled
	// confirmation putting the previous row back, from writing it straight
	// back (same shape as lumina_dialogs_style_settings.cpp).
	const auto syncing = container->lifetime().make_state<bool>(false);
	const auto sync = [=] {
		*syncing = true;
		group->setValue(CurrentDuressMode());
		*syncing = false;
	};
	group->setChangedCallback([=](DuressMode mode) {
		if (*syncing || (mode == CurrentDuressMode())) {
			return;
		} else if (!IsLogoutMode(mode)) {
			SetDuressMode(mode);
			return;
		}
		ConfirmLogoutMode(controller, mode, crl::guard(container, [=](
				bool confirmed) {
			if (confirmed) {
				SetDuressMode(mode);
			} else {
				sync();
			}
		}));
	});
	FakeCrashChanges(
	) | rpl::on_next(sync, container->lifetime());

	Ui::AddSkip(container, st::settingsSendTypeSkip);
}

// One reactive paragraph rather than a stack of conditional dividers: the
// warnings come and go as the code, the toggle, the mode, the passcode and
// the vault change, and a block of dividers appearing and disappearing under
// the rows reads far worse than a paragraph that grows a sentence.
[[nodiscard]] QString AboutText(bool codeIsPasscode) {
	auto lines = QStringList();
	lines.append(Tr(u"LuminaSecurityFakeCrashInfo"_q));
	const auto enabled = FakeCrashEnabled();
	const auto mode = CurrentDuressMode();
	if (enabled) {
		if (IsLogoutMode(mode)) {
			// The owner's warning for the chosen mode stays in view for as
			// long as it is armed, not only in the confirmation.
			lines.append(LogoutWarning(mode));
			lines.append(Tr(u"LuminaDuressLocalDataWarning"_q));
			lines.append(Tr(u"LuminaDuressDeferredInfo"_q));
		} else {
			lines.append(Tr(u"LuminaFakeCrashModeExitInfo"_q));
		}
	}

	// The decision recorded in the port plan (§6 Q3): a fingerprint opens the
	// app without the passcode field ever being touched, and would therefore
	// walk straight past the duress code. Said here because the toggle that
	// causes it is here, and the system-unlock switch that stops working is
	// on a different page entirely.
	lines.append(Tr(u"LuminaSecurityFakeCrashBiometricInfo"_q));

	if (enabled && FakeCrashCode().isEmpty()) {
		lines.append(Tr(u"LuminaSecurityFakeCrashNoCodeInfo"_q));
	}
	// Nowhere to type the code: no lock screen, and - for a logout mode,
	// which the vault also accepts - no vault either.
	if (enabled
		&& !HasLocalPasscode()
		&& !(IsLogoutMode(mode) && VaultArmed())) {
		lines.append(Tr(u"LuminaSecurityFakeCrashNoPasscodeInfo"_q));
	}
	if (FakeCrashArmed() && codeIsPasscode) {
		// This page refuses a duress code equal to the passcode, and the
		// passcode page now refuses a passcode equal to the duress code, so
		// this is only reachable through a restored backup or a passcode
		// changed to the duress code before the passcode page checked it.
		lines.append(Tr(u"LuminaSecurityFakeCrashCodeClashInfo"_q));
	}
	if (FakeCrashArmed() && SameAsVaultCode(FakeCrashCode())) {
		// Only reachable through a restored backup: both editors refuse it.
		lines.append(Tr(u"LuminaSecurityFakeCrashVaultClashInfo"_q));
	}
	return lines.join(u" "_q);
}

} // namespace

void AddFakeCrashRows(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		TrValue(u"LuminaSecurityFakeCrashHeader"_q));
	AddEnableRow(container);

	// Built once and toggled reactively, like the provider blocks on the
	// translation page: switching the toggle never rebuilds the block and
	// never invalidates a pointer another row is holding.
	const auto codeBlock = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container))
	)->toggleOn(
		rpl::single(
			rpl::empty
		) | rpl::then(
			Changes()
		) | rpl::map([] {
			return FakeCrashEnabled();
		})
	)->finishAnimating()->entity();
	AddValueRow(
		codeBlock,
		TrValue(u"LuminaSecurityFakeCrashCode"_q),
		[] {
			return Tr(FakeCrashCode().isEmpty()
				? u"LuminaSecurityFakeCrashCodeNotSet"_q
				: u"LuminaSecurityFakeCrashCodeSet"_q);
		},
		[=] { controller->show(Box(EditCodeBox)); });
	AddModeRows(codeBlock, controller);

	// The clash warning needs a passcode derivation, which since 7.3 runs on
	// a worker (see CheckCodeIsLocalPasscode): the paragraph shows the last
	// verdict and is rebuilt when a new one arrives. Every change that can
	// move it starts a new check; a generation counter drops a verdict that
	// arrives for a state that has changed since.
	const auto codeIsPasscode = container->lifetime().make_state<
		rpl::variable<bool>>(false);
	const auto generation = container->lifetime().make_state<int>(0);
	rpl::single(
		rpl::empty
	) | rpl::then(
		Changes()
	) | rpl::on_next([=] {
		const auto now = ++*generation;
		if (!FakeCrashArmed()) {
			*codeIsPasscode = false;
			return;
		}
		CheckCodeIsLocalPasscode(FakeCrashCode(), container, [=](bool clash) {
			if (*generation == now) {
				*codeIsPasscode = clash;
			}
		});
	}, container->lifetime());

	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::combine(
			rpl::single(rpl::empty) | rpl::then(Changes()),
			codeIsPasscode->value()
		) | rpl::map([](rpl::empty_value, bool clash) {
			return AboutText(clash);
		}));
}

} // namespace Lumina
