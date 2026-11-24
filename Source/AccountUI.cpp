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
void drawAvatar (juce::Graphics& g, juce::Rectangle<float> area, const juce::Image& picture, const juce::String& email, bool pro)
{
    const float d = juce::jmin (area.getWidth(), area.getHeight());
    const auto circle = juce::Rectangle<float> (d, d).withCentre (area.getCentre());

    if (pro) // glowing ring for Pro
    {
        g.setColour (Theme::accent.withAlpha (0.35f));
        g.fillEllipse (circle.expanded (2.5f));
    }

    if (picture.isValid())
    {
        juce::Graphics::ScopedSaveState s (g);
        juce::Path clip;
        clip.addEllipse (circle);
        g.reduceClipRegion (clip);
        g.setOpacity (1.0f); // images use the current colour's alpha - the glow above left it at 35%
        g.drawImage (picture, circle, juce::RectanglePlacement::fillDestination);
    }
    else
    {
        g.setGradientFill (juce::ColourGradient (Theme::accent2, circle.getX(), circle.getY(),
                                                 juce::Colour (0xff5a3fd8), circle.getRight(), circle.getBottom(), false));
        g.fillEllipse (circle);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (d * 0.48f, juce::Font::bold));
        g.drawText (email.isNotEmpty() ? email.substring (0, 1).toUpperCase() : juce::String ("?"), circle.toNearestInt(), juce::Justification::centred);
    }

    g.setColour (pro ? Theme::accent2 : juce::Colours::white.withAlpha (0.35f));
    g.drawEllipse (circle.reduced (0.5f), pro ? 1.6f : 1.0f);
}

void AccountBadge::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const float rad = juce::jmin (r.getHeight() * 0.5f, 10.0f);

    if (pro)
    {
        g.setColour (Theme::accent.withAlpha (0.28f));
        g.fillRoundedRectangle (r.expanded (2.0f), rad + 2.0f);
        g.setGradientFill (juce::ColourGradient (Theme::accent.brighter (0.2f), r.getX(), r.getY(), Theme::accent.darker (0.4f), r.getX(), r.getBottom(), false));
    }
    else
    {
        g.setColour (juce::Colours::white.withAlpha (down ? 0.03f : (over ? 0.13f : 0.07f)));
    }
    g.fillRoundedRectangle (r, rad);
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (pro ? 0.25f : 0.12f), r.getX(), r.getY(),
                                             juce::Colours::white.withAlpha (0.0f), r.getX(), r.getCentreY(), false));
    g.fillRoundedRectangle (r, rad);
    g.setColour (juce::Colours::white.withAlpha (over ? 0.32f : 0.16f));
    g.drawRoundedRectangle (r.reduced (0.5f), rad, 1.0f);

    auto content = r.reduced (5.0f, 4.0f);
    if (showAvatar)
    {
        drawAvatar (g, content.removeFromLeft (content.getHeight()), pro ? avatar : juce::Image(), email, pro);
        content.removeFromLeft (6.0f);
    }
    g.setColour (pro ? juce::Colours::white : Theme::text);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText (text, content.toNearestInt(), showAvatar ? juce::Justification::centredLeft : juce::Justification::centred, true);
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

    changePicBtn.setTooltip ("Pick a PNG or JPG - it's cropped to a circle (Pro)");
    changePicBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a profile picture",
                                                       juce::File::getSpecialLocation (juce::File::userPicturesDirectory),
                                                       "*.png;*.jpg;*.jpeg");
        juce::Component::SafePointer<AccountPanel> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
        {
            if (safe != nullptr && fc.getResult().existsAsFile())
                safe->account.setAvatarFromFile (fc.getResult());
        });
    };
    removePicBtn.onClick = [this] { account.removeAvatar(); };

    for (juce::Component* c : std::initializer_list<juce::Component*> { &emailBox, &passwordBox, &signInBtn, &signUpBtn, &forgotBtn,
                                                                         &refreshBtn, &upgradeBtn, &signOutBtn, &changePicBtn, &removePicBtn })
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
    changePicBtn.setVisible (configured && signedIn && account.isPro());
    changePicBtn.setEnabled (! busy);
    removePicBtn.setVisible (configured && signedIn && account.isPro() && account.getAvatar().isValid());
    removePicBtn.setEnabled (! busy);
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
        const bool pro = account.isPro();

        // identity row: profile picture + email (+ picture buttons, laid out in resized())
        auto idRow = r.removeFromTop (66);
        avatarArea = idRow.removeFromLeft (60);
        drawAvatar (g, avatarArea.toFloat().reduced (2.0f), account.getAvatar(), account.getEmail(), pro);
        idRow.removeFromLeft (12);
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Signed in as", idRow.removeFromTop (16), juce::Justification::left);
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        g.drawText (account.getEmail(), idRow.removeFromTop (22), juce::Justification::left, true);
        if (! pro)
        {
            g.setColour (Theme::accent);
            g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
            g.drawText ("Profile pictures come with Pro", idRow.removeFromTop (22), juce::Justification::left, true);
        }
        r.removeFromTop (12);

        // plan badge
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
        g.drawText (account.isUltra() ? "ULTRA PRO" : (pro ? "PRO" : "FREE"), badge.reduced (16.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft);
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
        // picture buttons under the email, beside the avatar (matches paint())
        auto idRow = r.removeFromTop (66).withTrimmedLeft (72);
        idRow.removeFromTop (40);
        auto picRow = idRow.removeFromTop (24);
        changePicBtn.setBounds (picRow.removeFromLeft (130));
        picRow.removeFromLeft (6);
        removePicBtn.setBounds (picRow.removeFromLeft (76));

        auto bottom = getLocalBounds().reduced (30, 22).removeFromBottom (36);
        signOutBtn.setBounds (bottom.removeFromRight (100));
        bottom.removeFromRight (8);
        refreshBtn.setBounds (bottom.removeFromRight (130));
        bottom.removeFromRight (8);
        upgradeBtn.setBounds (bottom);
    }
}

//==============================================================================
ProPrompt::ProPrompt (Account& a, juce::String featureName, int requiredTier, std::function<void()> open)
    : account (a), feature (std::move (featureName)), tier (requiredTier), openAccount (std::move (open))
{
    const bool signedIn = account.isSignedIn();
    primaryBtn.setButtonText (! signedIn ? "Sign in / Create account"
                                         : (tier >= 2 ? "Get Ultra Pro (coming soon)" : "Get Pro (coming soon)"));
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

void ProPrompt::show (Account& account, const juce::String& featureName, int requiredTier, juce::Component* centreAround, std::function<void()> openAccount)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new ProPrompt (account, featureName, requiredTier, std::move (openAccount)));
    o.dialogTitle = requiredTier >= 2 ? "AMETHYST Ultra Pro" : "AMETHYST Pro";
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
    const bool ultraNeeded = tier >= 2;
    g.drawText (ultraNeeded ? "Unlock with Ultra Pro" : "Unlock with Pro", head, juce::Justification::centredLeft);

    r.removeFromTop (12);
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (14.5f));
    g.drawFittedText (feature + (ultraNeeded ? " is part of AMETHYST Ultra Pro." : " is part of AMETHYST Pro."),
                      r.removeFromTop (24), juce::Justification::left, 1);
    r.removeFromTop (8);
    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (13.0f));
    if (ultraNeeded)
        g.drawFittedText ("- AMETHYST TUNE real-time pitch correction\n- Everything in Pro (all AMETHYST effects, profile picture)\n"
                          "- Future Ultra-only tools",
                          r.removeFromTop (84), juce::Justification::topLeft, 6);
    else
        g.drawFittedText ("- " + proFeatures.joinIntoString ("\n- "), r.removeFromTop (84), juce::Justification::topLeft, 6);

    if (account.isSignedIn() && account.getTier() < tier)
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
