#!/bin/sh

set -e

# Notarizes each .dmg or .zip given, and staples the ticket to each .dmg
# (a zip can't hold one: staple the app inside and zip it again). The
# credentials are an App Store Connect API key, the .p8 file's contents in
# APPLE_NOTARY_KEY, with APPLE_NOTARY_KEY_ID and APPLE_NOTARY_ISSUER_ID,
# or an Apple ID: APPLE_NOTARIZATION_USERNAME, APPLE_NOTARIZATION_PASSWORD
# (an app-specific password) and APPLE_TEAM_ID. It waits NOTARY_TIMEOUT
# for each; Apple goes on after that, and the submission's ID, which it
# prints, finds the outcome later: xcrun notarytool info <ID>.

NOTARY_TIMEOUT=${NOTARY_TIMEOUT:-60m}

if [ -n "${APPLE_NOTARY_KEY}" ]
then
    KEY_FILE=$(mktemp)
    trap 'rm -f "${KEY_FILE}"' EXIT
    printf '%s\n' "${APPLE_NOTARY_KEY}" > "${KEY_FILE}"
elif [ -n "${APPLE_NOTARIZATION_USERNAME}" ]
then
    echo "Creating NotarizationProfile..."
    xcrun notarytool store-credentials --apple-id "${APPLE_NOTARIZATION_USERNAME}" \
        --password "${APPLE_NOTARIZATION_PASSWORD}" \
        --team-id "${APPLE_TEAM_ID}" "NotarizationProfile"
else
    echo "No notarization credentials supplied, skipping..."
    exit 0
fi

# notarytool with the credentials
notarytool()
{
    if [ -n "${KEY_FILE}" ]
    then
        xcrun notarytool "$@" --key "${KEY_FILE}" \
            --key-id "${APPLE_NOTARY_KEY_ID}" --issuer "${APPLE_NOTARY_ISSUER_ID}"
    else
        xcrun notarytool "$@" --keychain-profile "NotarizationProfile"
    fi
}

# the value at a key of the plist on standard input; nothing if it has none
plist_value()
{
    VALUE=$(plutil -extract "$1" raw -o - - 2>/dev/null) && printf '%s' "${VALUE}"
    return 0
}

if [ "$#" -eq 0 ]
then
    echo "Error: Please provide one or more .dmg or .zip files"
    exit 1
fi

for FILE in "$@"; do
    case ${FILE} in
        *.dmg|*.zip)
            if [ ! -f "${FILE}" ]
            then
                echo "Error: '${FILE}' does not exist or is not a regular file"
                exit 1
            fi

            echo "Submitting notarization request..."
            RESULT=$(notarytool submit "${FILE}" --output-format plist) || true
            ID=$(echo "${RESULT}" | plist_value id)
            if [ -z "${ID}" ]
            then
                echo "${RESULT}"
                echo "Error: couldn't submit '${FILE}' for notarization"
                exit 1
            fi
            echo "Submission ID: ${ID}"

            RESULT=$(notarytool wait "${ID}" --timeout "${NOTARY_TIMEOUT}" --output-format plist) || true
            STATUS=$(echo "${RESULT}" | plist_value status)
            case ${STATUS} in
                Accepted)
                    ;;
                ""|"In Progress")
                    echo "${RESULT}"
                    echo "Error: no outcome for '${FILE}' in ${NOTARY_TIMEOUT}; Apple may still notarize it: xcrun notarytool info ${ID}"
                    exit 1
                    ;;
                *)
                    echo "${RESULT}"
                    notarytool log "${ID}" || true
                    echo "Error: notarization of '${FILE}' failed: ${STATUS}"
                    exit 1
                    ;;
            esac

            case ${FILE} in
                *.dmg)
                    # the ticket can take a while to reach stapler's CDN
                    # after Accepted, so it's tried a few times
                    echo "Stapling..."
                    TRIES=1
                    until xcrun stapler staple "${FILE}"
                    do
                        if [ "${TRIES}" -ge 5 ]
                        then
                            echo "Error: '${FILE}' was notarized (${ID}), but stapling its ticket failed"
                            exit 1
                        fi
                        TRIES=$((TRIES + 1))
                        sleep 30
                    done
                    ;;
            esac
            ;;

        *)
            echo "Error: '${FILE}' does not have a .dmg or .zip extension"
            exit 1
            ;;
    esac
done
