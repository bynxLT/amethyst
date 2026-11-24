#include "AccountUI.h"

namespace
{
    void styleEditor (juce::TextEditor& ed, const juce::String& hint)
    {
        ed.setTextToShowWhenEmpty (hint, Theme::textDim.withAlpha (0.5f));
        ed.setFont (juce::FontOptions (15.0f));
        ed.setIndents (10, 7);
        ed.setJustification (juce::Justification::centredLeft);
    }

    void paintBackdrop (juce::Graphics& g, juce::Image& cache, juce::Component& c)
    {
        if (cache.isNull() || cache.getWidth() != c.getWidth() || cache.getHeight() != c.getHeight())
        {
            cache = juce::Image (juce::Image::RGB, juce::jmax (1, c.getWidth()), juce::jmax (1, c.getHeight()), false);
            juce::Graphics bg (cache);
            Glass::background (bg, c.getLocalBounds());
            Glass::panel (bg, c.getLocalBounds().toFloat().reduced (10.0f), 16.0f, 1.1f);
        }
        g.drawImageAt (cache, 0, 0);
    }

    const juce::StringArray proFeatures {
        "Facet EQ, Prism Dynamics, Geode Reverb, Crystal Echo",
        "Prism Mod, Lustre, Silk De-Esser, Ember",
        "Twin Voice and Shift",
        "All factory presets and future AMETHYST effects",
    };
}

//==============================================================================
AccountPanel::AccountPanel (Account& a) : account (a)
{
    styleEditor (emailBox, "Email");
    styleEditor (passwordBox, "Password");
    passwordBox.setPasswordCharacter ((juce::juce_wchar) 0x2022);
    emailBox.setText (account.getEmail(), false);
    passwordBox.onReturnKey = [this] { signInBtn.triggerClick(); };

    signInBtn.setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    signInBtn.setToggleState (true, juce::dontSendNotification); // glowing primary button
    signInBtn.onClick = [this] { account.signIn (emailBox.getText(), passwordBox.getText()); };
    signUpBtn.onClick = [this] { account.signUp (emailBox.getText(), passwordBox.getText()); };
    forgotBtn.onClick = [this] { account.sendPasswordReset (emailBox.getText()); };
    refreshBtn.onClick = [this] { account.refreshStatus(); };
    signOutBtn.onClick = [this] { account.signOut(); passwordBox.clear(); };
    upgradeBtn.setColour (juce::TextButton::buttonOnColourId, Theme::accent2);
    upgradeBtn.setToggleState (true, juce::dontSendNotification);
    upgradeBtn.onClick = []
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "AMETHYST Pro",
                                                "Subscriptions are coming soon!\n\nOnce they launch you'll be able to upgrade right here.");
    };

    for (juce::Component* c : std::initializer_list<juce::Component*> { &emailBox, &passwordBox, &signInBtn, &signUpBtn, &forgotBtn,
                                                                         &refreshBtn, &upgradeBtn, &signOutBtn })
        addChildComponent (c);

    account.addChangeListener (this);
    setSize (440, 420);
    refresh();
}

AccountPanel::~AccountPanel()
{
    account.removeChangeListener (this);
}

void AccountPanel::show (Account& account, juce::Component* centreAround)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new AccountPanel (account));
    o.dialogTitle = "AMETHYST Account";
    o.dialogBackgroundColour = Theme::windowBg;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = centreAround;
    o.launchAsync();
}

void AccountPanel::refresh()
{
    const bool configured = account.isConfigured();
    const bool signedIn = account.isSignedIn();
    const bool busy = account.isBusy();

    for (juce::Component* c : std::initializer_list<juce::Component*> { &emailBox, &passwordBox, &signInBtn, &signUpBtn, &forgotBtn })
    {
        c->setVisible (configured && ! signedIn);
        c->setEnabled (! busy);
    }
    for (juce::Component* c : std::initializer_list<juce::Component*> { &refreshBtn, &signOutBtn })
    {
        c->setVisible (configured && signedIn);
        c->setEnabled (! busy);
    }
    upgradeBtn.setVisible (configured && signedIn && ! account.isPro());
    resized();
    repaint();
}

void AccountPanel::paint (juce::Graphics& g)
{
    paintBackdrop (g, bg, *this);
    auto r = getLocalBounds().reduced (30, 26);

    auto head = r.removeFromTop (54);
    drawLogoMark (g, head.removeFromLeft (46).toFloat().reduced (2.0f));
    head.removeFromLeft (10);
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (21.0f, juce::Font::bold));
    g.drawText ("AMETHYST", head.removeFromTop (28), juce::Justification::bottomLeft);
    g.setColour (Theme::accent);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("ACCOUNT", head, juce::Justification::topLeft);

    r.removeFromTop (14);

    if (! account.isConfigured())
    {
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (14.0f));
        g.drawFittedText ("Sign-in isn't set up in this build yet.\n\nAdd your Supabase project URL and anon key to "
                          "config/supabase.json and rebuild. See README.",
                          r.removeFromTop (120), juce::Justification::topLeft, 6);
        return;
    }

    if (account.isSignedIn())
    {
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (12.5f));
        g.drawText ("Signed in as", r.removeFromTop (18), juce::Justification::left);
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
        g.drawText (account.getEmail(), r.removeFromTop (26), juce::Justification::left, true);
        r.removeFromTop (12);

        // plan badge
        const bool pro = account.isPro();
        auto badge = r.removeFromTop (54).toFloat();
        if (pro)
        {
            g.setGradientFill (juce::ColourGradient (Theme::accent.withAlpha (0.5f), badge.getX(), badge.getY(),
                                                     Theme::accent2.withAlpha (0.35f), badge.getRight(), badge.getY(), false));
            g.fillRoundedRectangle (badge, 12.0f);
        }
        else
        {
            Glass::well (g, badge, 12.0f);
        }
        g.setColour (juce::Colours::white.withAlpha (pro ? 0.5f : 0.15f));
        g.drawRoundedRectangle (badge.reduced (0.5f), 12.0f, 1.0f);
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        g.drawText (pro ? "PRO" : "FREE", badge.reduced (16.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft);
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (12.5f));
        g.drawText (account.getPlanText(), badge.reduced (16.0f, 0.0f).toNearestInt(), juce::Justification::centredRight, true);

        if (! pro)
        {
            r.removeFromTop (14);
            g.setColour (Theme::textDim);
            g.setFont (juce::FontOptions (12.5f));
            g.drawFittedText ("Pro unlocks:\n- " + proFeatures.joinIntoString ("\n- "), r.removeFromTop (92), juce::Justification::topLeft, 6);
        }
    }

    // status line
    g.setColour (account.isBusy() ? Theme::accent2 : Theme::textDim);
    g.setFont (juce::FontOptions (12.5f));
    g.drawFittedText (account.getStatus(), getLocalBounds().reduced (30, 0).removeFromBottom (58).removeFromTop (34),
                      juce::Justification::centredLeft, 2);
}

void AccountPanel::resized()
{
    bg = {};
    auto r = getLocalBounds().reduced (30, 26);
    r.removeFromTop (68);

    if (! account.isSignedIn())
    {
        emailBox.setBounds (r.removeFromTop (38));
        r.removeFromTop (10);
        passwordBox.setBounds (r.removeFromTop (38));
        r.removeFromTop (16);
        auto row = r.removeFromTop (36);
        signInBtn.setBounds (row.removeFromLeft (row.getWidth() / 2 - 5));
        row.removeFromLeft (10);
        signUpBtn.setBounds (row);
        r.removeFromTop (8);
        forgotBtn.setBounds (r.removeFromTop (28).withSizeKeepingCentre (150, 26));
    }
    else
    {
        auto bottom = getLocalBounds().reduced (30, 22).removeFromBottom (36);
        signOutBtn.setBounds (bottom.removeFromRight (100));
        bottom.removeFromRight (8);
        refreshBtn.setBounds (bottom.removeFromRight (130));
        bottom.removeFromRight (8);
        upgradeBtn.setBounds (bottom);
    }
}

//==============================================================================
ProPrompt::ProPrompt (Account& a, juce::String featureName, std::function<void()> open)
    : account (a), feature (std::move (featureName)), openAccount (std::move (open))
{
    const bool signedIn = account.isSignedIn();
    primaryBtn.setButtonText (! signedIn ? "Sign in / Create account" : "Get Pro (coming soon)");
    primaryBtn.setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    primaryBtn.setToggleState (true, juce::dontSendNotification);
    primaryBtn.onClick = [this]
    {
        auto cb = openAccount;
        close();
        if (cb)
            juce::MessageManager::callAsync (cb);
    };
    laterBtn.onClick = [this] { close(); };
    addAndMakeVisible (primaryBtn);
    addAndMakeVisible (laterBtn);
    setSize (440, 340);
}

void ProPrompt::show (Account& account, const juce::String& featureName, juce::Component* centreAround, std::function<void()> openAccount)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new ProPrompt (account, featureName, std::move (openAccount)));
    o.dialogTitle = "AMETHYST Pro";
    o.dialogBackgroundColour = Theme::windowBg;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = centreAround;
    o.launchAsync();
}

void ProPrompt::close()
{
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
        dw->exitModalState (0);
}

void ProPrompt::paint (juce::Graphics& g)
{
    paintBackdrop (g, bg, *this);
    auto r = getLocalBounds().reduced (30, 26);

    auto head = r.removeFromTop (50);
    drawLogoMark (g, head.removeFromLeft (44).toFloat().reduced (2.0f));
    head.removeFromLeft (10);
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText ("Unlock with Pro", head, juce::Justification::centredLeft);

    r.removeFromTop (12);
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (14.5f));
    g.drawFittedText (feature + " is part of AMETHYST Pro.", r.removeFromTop (24), juce::Justification::left, 1);
    r.removeFromTop (8);
    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (13.0f));
    g.drawFittedText ("- " + proFeatures.joinIntoString ("\n- "), r.removeFromTop (84), juce::Justification::topLeft, 6);

    if (account.isSignedIn() && ! account.isPro())
    {
        g.setColour (Theme::accent2);
        g.drawFittedText ("Subscriptions launch soon. Already upgraded? Open your account and click Refresh status.",
                          r.removeFromTop (40), juce::Justification::topLeft, 2);
    }
}

void ProPrompt::resized()
{
    bg = {};
    auto bottom = getLocalBounds().reduced (30, 24).removeFromBottom (36);
    laterBtn.setBounds (bottom.removeFromRight (100));
    bottom.removeFromRight (10);
    primaryBtn.setBounds (bottom);
}
