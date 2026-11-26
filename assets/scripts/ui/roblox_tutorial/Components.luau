--!strict
local React = require("React")

local function MyColorfulClickableSquare()
    local color, setColor = React.useState(Color3.new(255,255,255))
    return React.createElement("TextButton", {
        Size = UDim2.new(0,200,0,200),
        BackgroundColor3 = color,
		TextXAlignment = Enum.TextXAlignment.Center,
		TextYAlignment = Enum.TextYAlignment.Center,
		Text = "Click me to change color!",
		[React.Event.Activated] = function() 
			local newColor = Color3.fromRGB(math.random(0,255), math.random(0,255), math.random(0,255))
			print("changing color to " .. tostring(newColor) .. "!")
			setColor(newColor)
		end
    })
end

local function MyBoopMachine()
	local boops, setBoops = React.useState({
		nose = false,
		ears = false,
		tail = false,
	})

	local whatToBoop = React.useMemo(function()
		if not boops.nose then
			return "nose"
		elseif not boops.ears then
			return "ears"
		elseif not boops.tail then
			return "tail"
		else
			return "all booped!"
		end
	end, {boops})

	return React.createElement("TextButton", {
        Size = UDim2.new(0,200,0,200),
        BackgroundColor3 = Color3.fromRGB(255,255,0),
		Text = "boop " .. whatToBoop,
		[React.Event.Activated] = function() 
			local boopCopy = {
				nose = boops.nose,
				ears = boops.ears,
				tail = boops.tail,
			}
			boopCopy[whatToBoop] = true
			setBoops(boopCopy)
		end
    })
end

local function useToggleState(default: boolean): {
	enabled: boolean,
	enable: () -> (),
	disable: () -> (),
}
	local enabled, setEnabled = React.useState(default)

	local enable = React.useCallback(function()
		setEnabled(true)
	end, {})

	local disable = React.useCallback(function()
		setEnabled(false)
	end, {})

	return {
		enabled = enabled,
		enable = enable,
		disable = disable,
	}
end

local function MyToggleWoggle()
	local toggle = useToggleState(false)
	return React.createElement("TextButton", {
		Size = UDim2.new(0,200,0,200),
		BackgroundColor3 = toggle.enabled and Color3.fromRGB(255,194,132) or Color3.fromRGB(255,106,106),
		Text = toggle.enabled and "Potato" or "Tomato",
		[React.Event.Activated] = function()
			if toggle.enabled then
				toggle.disable()
			else
				toggle.enable()
			end
		end
	})
end

export type MyReallyReallyCuteFrameProps = {
	child: React.ReactElement,
}

local function MyReallyReallyCuteFrame(props: MyReallyReallyCuteFrameProps)
	return React.createElement("Frame", {
		Size = UDim2.new(0,200,0,200),
		AnchorPoint = Vector2.new(0.5,0.5),
		Position = UDim2.new(0.5,0,0.5,0),
		BackgroundColor3 = Color3.fromRGB(255,194,132),
	}, {
		React.createElement("UIPadding", {
			PaddingTop = UDim.new(0,20),
			PaddingBottom = UDim.new(0,20),
			PaddingLeft = UDim.new(0,20),
			PaddingRight = UDim.new(0,20),
		}),
		React.createElement("UICorner", {
			CornerRadius = UDim.new(0,20)
		}),
		React.createElement("UIStroke", {
			Thickness = 8,
			Color = Color3.fromRGB(255,106,106),
		}),
		Content = props.child,
	})
end

return {
    MyColorfulClickableSquare = MyColorfulClickableSquare,
    MyBoopMachine = MyBoopMachine,
    MyToggleWoggle = MyToggleWoggle,
    MyReallyReallyCuteFrame = MyReallyReallyCuteFrame,
}
